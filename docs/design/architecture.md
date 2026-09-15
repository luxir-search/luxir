# Luxir Architecture

Luxir is a hybrid search engine: full-text relevance, vector similarity, and
faceted analytics in one native-code core, queried through one composable
request tree.
For a class-by-class map of the source tree, see
[../dev/codebase-map.md](../dev/codebase-map.md).

## Design goals

Luxir starts from the economics of cloud compute. Inefficiency is a
recurring cost rather than a one-time hardware purchase, and instance pricing
is roughly linear up to very large machines, so one big node is now as
economical a unit of capacity as a cluster of small ones. Both facts favor a
single efficient process that can use an entire modern machine.

- **Built for modern hardware.** Many cores, large memories, fast NVMe, and
  SIMD are baseline assumptions: work-stealing parallelism throughout,
  memory-mapped immutable data, vectorized codecs, and allocation discipline
  on every hot path.
- **Scales up before it scales out.** One process is designed around large
  machines, many cores, and a memory-mapped address space, with no managed-heap
  ceiling or per-node coordination tax. Replication and scale-to-zero are
  separate future layers rather than reasons to shard first.
- **One request tree.** Lexical queries, vector similarity, filters, facets,
  statistics, and fusion are nodes in one request tree. A result page with
  ranking and analytics is one request and one round trip.
- **Frugal with memory.** Native code with no garbage collector, arenas and
  pools instead of general-purpose allocation on hot paths, and an index
  served from memory-mapped files.

## Data model

Documents live in **collections**. A collection is stored as a **shard**
holding one index; an index is a set of **immutable segments** plus a small
metadata file naming the current commit point.

Most of what follows depends on segments being immutable:

- Writers never modify what readers are using. A search pins a consistent
  snapshot for its whole lifetime; new commits swap in atomically without
  pausing queries.
- Crash safety comes from the write protocol: files are written to a
  temporary name, synced, and atomically renamed, and a commit point is
  published only after the files it names are durable. After a crash the
  index reopens at the previous commit.
- Compaction is concurrent merging: merges produce new segments in the
  background and never stall ingest or search.

## Designed for modern hardware

### Work-stealing parallelism

All parallelism runs on a work-stealing task scheduler, which keeps
every core busy even when work is skewed.

- **Search** parallelizes across segments (and within segments) and
  across the independent operations in
  a request; cores that finish small segments steal work from large ones.
- **Indexing** is a flow-graph pipeline: document processing, inversion,
  segment flushing, merging, and commit sequencing are independent stages.
  Ingest never waits behind a merge, and a commit does not stop the world.
  Independent updates are automatically parallelized. A single HTTP NDJSON
  connection submits multiple internal batches for concurrent indexing, with
  a bounded number in flight and backpressure when that limit is reached.
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

- HTTP uses one single-runner `io_context` per configured connection shard,
  plus a dedicated accept context and thread. Successful accepts go to the
  shard with the fewest live connections and the connection stays pinned to
  that shard; an idle shard parks independently and does no per-request work
  for active shards. The gRPC server runs on completion queues.
- Search work defaults to serial execution inline on the connection thread
  that received it: cheap queries pay no scheduler handoff, and request rate
  never wakes idle workers. A request can instead opt onto the shared
  work-stealing scheduler (`max_parallel != 0`) so an expensive query does
  not occupy its connection thread. The gRPC unary update path currently
  waits for indexing on its completion-queue handler.
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
complete match domain; facets can own per-bucket metrics, nested facets, and a
ranked document list per bucket; fusion consumes ranked top-docs sources and
can carry facets over the fused set. One request describes the whole page you
want to render; the engine executes its named operations in parallel over the
same index view.

Execution profiling follows that tree rather than wrapping the engine in a
black-box timer. Instrumented operations emit per-segment strategy, selection
inputs, timing, and human-readable decisions on request. Profiling is opt-in;
string facets are the first instrumented operation, and other nodes can adopt
the same hook without changing dispatch or response assembly.

Execution adapts to what the request actually consumes. A top-k request
that does not ask for an exact total runs with block-max pruning: postings
carry per-block score upper bounds, and a MaxScore-based scorer skips
blocks and documents that cannot reach the current top k. A request that
consumes an exact count runs exhaustively, because an exact count requires
it. The choice is made per request from what the request asks for; there is
no separate accuracy setting.

Vector search composes the same way. A kNN query is just a query, with a
scorer that participates in the boolean tree like any term. Filters apply
inside the vector search rather than as a post-pass over a fixed unfiltered
top-k, and hybrid ranking combines lexical and vector evidence in a single
request.

Filters are cached per collection as document sets. Entries are ranked by
how expensive they are to rebuild per byte, so under pressure a cheap
single-term filter is evicted before a compound boolean or a kNN membership
set. Cached sets are consumed in
bulk windows, which is faster than re-decoding postings even for a filter that
is a single term.

## Vector search, natively integrated

Vectors are stored as ordinary columns. Exact kNN scans the column;
approximate search uses per-segment ANN indexes (currently IVF+PQ) built as
segment overlays, which follow the segment lifecycle and are served
zero-copy from memory-mapped files like everything else. Approximate
candidates are rescored at full precision. Because the ANN index is part of
the segment, there is no separate vector store to keep in sync with your
documents: deletes, updates, and merges apply to vectors the same way they
apply to text.

See [vector-search.md](vector-search.md) for the overlay internals and
[../guide/vector-search.md](../guide/vector-search.md) for the user
contract.

## API surfaces

The engine speaks gRPC and JSON over HTTP. Both share the protobuf message
vocabulary and semantic model, while HTTP applies its own public JSON dialect
designed to be written by hand. Requests are structured; wherever a request
takes a query, the query can also be written in the Luxir query language,
and both forms build the same tree. Unknown keys are reported with positions,
so a typo is an error rather than silently ignored, and `?explain=request`
echoes the canonical structured form of whatever was sent. Ingest streams
over gRPC or HTTP (NDJSON).
