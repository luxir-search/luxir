# Solux Architecture

Solux is a hybrid search engine: full-text relevance, vector similarity, and
faceted analytics in one native-code core, queried through one composable
request tree.
For a class-by-class map of the source tree, see
[../dev/codebase-map.md](../dev/codebase-map.md).

## Design goals

Solux started from a premise about cloud economics: compute is no longer a
sunk cost. You pay for inefficiency every month, forever - and instance
pricing is linear up to very large machines, so one big node is now the
economical unit of capacity that clusters of small machines once were. Both
facts reward deep engineering investment in a single efficient process that
can use an entire modern machine.

- **Built for modern hardware.** Many cores, large memories, fast NVMe, and
  SIMD are assumptions, not afterthoughts: work-stealing parallelism
  throughout, memory-mapped immutable data, vectorized codecs, and
  allocation discipline on every hot path.
- **Scales up before it scales out.** One process is designed around large
  machines, many cores, and a memory-mapped address space, with no managed-heap
  ceiling or per-node coordination tax. Maximum validated machine size and
  workload-specific throughput are not yet published claims.
  Capacity comes from scaling up first. Replication and scale-to-zero are
  separate future layers rather than reasons to shard first; neither is
  shipped in the current single-node server.
- **One engine, one algebra.** Lexical queries, vector similarity, filters,
  facets, statistics, and fusion are nodes in one request tree. A result page
  with ranking and analytics is one request and one round trip.
- **Frugal with memory.** Native code with no garbage collector, arenas and
  pools instead of general-purpose allocation on hot paths, and an index
  served from memory-mapped files.

## Data model

Documents live in **collections**. A collection is stored as a **shard**
holding one index; an index is a set of **immutable segments** plus a small
metadata file naming the current commit point.

Immutability is the load-bearing decision:

- Writers never modify what readers are using. A search pins a consistent
  snapshot for its whole lifetime; new commits swap in atomically without
  pausing queries.
- Crash safety is structural: files are written to a temporary name, synced,
  and atomically renamed, and a commit point is published only after the
  files it names are durable. A crash lands on the previous commit, never in
  between.
- Compaction is concurrent merging: merges produce new segments in the
  background and never stall ingest or search.

## Designed for modern hardware

### Work-stealing parallelism

All parallelism runs on a work-stealing task scheduler, which keeps
every core busy even when work is skewed.

- **Search** parallelizes across segments (and within segments) and
  across the independent ops of
  a request; cores that finish small segments steal work from large ones.
- **Indexing** is a flow-graph pipeline: document processing, inversion,
  segment flushing, merging, and commit sequencing are independent stages.
  Ingest never waits behind a merge, and a commit does not stop the world.
  Independent updates are automatically parallelized. The HTTP NDJSON path
  currently allows one internal batch in flight per connection, so several
  producer streams are needed to saturate a many-core host.
- **Merging** parallelizes inside a single merge, not just across merges:
  every field merges as its own task, because the segment format does not
  tie index structures to specific files - concurrent tasks write their own
  output streams with no coordination. Tasks are admitted against a
  node-wide indexing-RAM budget: merge memory is predictable up front from
  segment statistics, so heavy fields are priced exactly, scheduled
  largest-first, and light column merges pack around them.

### Asynchronous network IO

Both API surfaces multiplex connections with event loops. Search and streaming
response paths keep connection handling separate from long-running engine work.

- A small pool of io threads multiplexes all connections: the HTTP server
  is an asynchronous proactor, and the gRPC server runs
  on completion queues.
- Search work is dispatched onto the work-stealing scheduler, so a long query
  cannot starve the network and a busy network cannot starve queries. The gRPC
  unary update path currently waits for indexing on its completion-queue
  handler; it is the exception to the non-blocking transport model.
- Ingest is streaming and incremental: NDJSON bodies are parsed as bytes
  arrive and each document enters the indexing pipeline immediately - a
  document can be getting inverted while the request that carried it is
  still on the wire. Responses stream back the same way, a chunk at a time.
  For HTTP/JSON clients, there is no maximum stream size and no required
  user-visible bulk-request boundary.

### Memory-mapped, zero-copy reads

Segments are read via mmap, and the on-disk format is the in-memory format:
postings, columns, and vector index data are consumed in place, with no
deserialization step and no buffer copies between the page cache and query
execution. The OS page cache is the caching layer - a hot index is served
from memory, a cold one faults in on demand.

### SIMD and branch discipline

- Postings and numeric data are compressed with SIMD codecs (FastPFOR-based
  bit-packing, StreamVByte).
- Document sets use Screaming bitsets, a roaring-style two-level bitmap
  tuned for the engine's access patterns.
- Hot lookup paths use branchless binary search where measurement shows it
  wins.
- Relevance scoring is written so the hot loops auto-vectorize (BM25 over
  blocks of postings).

### Allocation discipline

- Indexing runs on rollback-capable memory pools; per-document work stays
  off the general-purpose allocator.
- Requests decode into arena-backed, non-owning message objects: no
  per-field heap allocations on the way in, trivially destructible on the
  way out.

## Search execution

A request is a tree of named **ops** - top-docs, facets, statistics, and
fusion - over a query tree. Top-docs can own facets and statistics over their
complete match domain; string/ID facets can own per-bucket metrics and nested
string/ID facets; fusion consumes ranked top-docs sources. One request
describes the whole page you want to render; the engine executes its named
operations in parallel over the same index view.

Execution profiling follows that tree rather than wrapping the engine in a
black-box timer. Instrumented operations emit per-segment strategy, selection
inputs, timing, and human-readable decisions on request. Profiling is opt-in;
string facets are the first instrumented operation, and other nodes can adopt
the same hook without changing dispatch or response assembly.

Execution adapts to what the request actually consumes. A top-k request
that does not ask for an exact total runs with block-max pruning: postings
carry per-block score upper bounds, and a MaxScore-based scorer skips
blocks and documents that cannot reach the current top k. A request that
consumes an exact count runs exhaustively, because that is the only honest
way to count. The choice is per-request and follows from the contract, not
from a hidden accuracy knob.

Vector search composes the same way. A kNN query is just a query, with a
scorer that participates in the boolean tree like any term. Filters apply
inside the vector search rather than as a post-pass over a fixed unfiltered
top-k, and hybrid ranking combines lexical and vector evidence in a single
request.

## Vector search, natively integrated

Vectors are stored as ordinary columns. Exact kNN scans the column;
approximate search uses per-segment ANN indexes (currently IVF+PQ) built as
segment overlays, which follow the segment lifecycle and are served
zero-copy from memory-mapped files like everything else. Approximate
candidates are rescored at full precision. Because the ANN index is part of
the segment, there is no second system to keep in sync with your documents:
deletes, updates, and merges apply to vectors the same way they apply to
text.

See [vector-search.md](vector-search.md) for the overlay internals and
[../guide/vector-search.md](../guide/vector-search.md) for the user
contract.

## API surfaces from one source of truth

The engine speaks gRPC and JSON over HTTP. Both share the protobuf message
vocabulary and semantic model, while HTTP deliberately applies its own public
JSON dialect. That dialect is human-writable and template-friendly,
snake_case throughout, uses untagged values, and reports unknown keys with
positions so typos are caught instead of ignored. Not every protobuf value has
an HTTP spelling yet; vector document values are the notable current gap.
Shorthand request forms echo back as their canonical structured equivalents
(`?explain=request`), so the API teaches the API. Ingest streams over gRPC or
HTTP (NDJSON).
