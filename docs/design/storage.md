# Storage and Object Stores

Status: design direction. Steps 1 and 2 below are built; steps 3 and 4 are
not. This page records the plan and the contract that work on either step has
to keep, so that contributions can be checked against it.

## Roadmap

1. Standalone: a node keeps its collections on a local filesystem
   (`--store.backend=fs`) or in RAM (`--store.backend=ram`).
2. Replication: followers copy finished commits from a writer
   ([replication guide](../guide/replication.md)).
3. Object store for durability, and as a source that followers can replicate
   from.
4. The object store as a source that nodes read from, with a local cache over
   it that can cache just needed parts.

The order is the original plan, not a requirement. Work on step 4 can come
first as long as it does not preclude step 3. In practice that means both
steps use the publication contract below.

## Publication contract

Steps 3 and 4 share one layout in the store:

- Files are immutable and named by file name plus content digest (xxh3).
  A file that exists in the store never changes.
- Commit manifests are immutable objects.
- Each collection has one HEAD that names its current manifest. HEAD is the
  only object that is ever overwritten. Updates are conditional writes that
  carry the writer's epoch, so a stale writer cannot publish over a newer one.

Writes stay local. The writer indexes, flushes, merges and commits to its
local storage as it does today. A syncer per collection publishes finished
snapshots in order: the files the store does not have yet, then the
manifest, then a conditional update of HEAD. While a snapshot is being
published it is pinned, the same way a follower's fetch pins it. If the
syncer falls behind, it finishes the snapshot it is on and then moves to the
newest one, so snapshots that were superseded in between are never uploaded.

Remote durability is reported separately from local visibility. Flushes and
commits do not wait for the store. A commit request can ask to wait until
HEAD covers it.

## Catalog ownership

Each tenant's CATALOG records `nodes`, a map from node id to process-lifetime
UUID. If two processes use one id on a store without a node lock, the newer
process replaces its lifetime at startup in every tenant that already mentions
the id, before cleanup or opening writers. The older process's mutations and
STATE claims fail with UNAVAILABLE, and it logs the takeover once per tenant.
A tenant not mentioning the id is stamped lazily: a zombie that first touches a
tenant the replacement never touched holds it until the replacement restarts.
Exclusive startup resets the map to just its current node and lifetime.

## Local storage and the cache

Once a file is published, its local copy is a cache entry and can be
evicted, since it can be fetched again by name and digest. Local storage on
any node holds the files that are not published yet plus a cache of
published files. For log-style data that rolls over to a new collection
periodically, a writer keeps only the collection it is writing, and older
collections are closed and read from the store, so its local storage scales
with recent data rather than total history.

Local storage can be RAM. A RAM-backed writer has no disk at all, and the
store is its durable copy. Data that is not published yet is lost if the
process dies, so commits that must survive a crash wait for remote
durability.

## Remote readers and retention

Superseded commits and their files are kept for X from local publication of
their successor, after its STATE tip is installed. Holds use a steady clock;
commit timestamps and publication stalls do not shorten the window. Readers on
other nodes must refresh their view more often than X. `--store.retention-ms`
sets X: by default 0 on exclusive stores and 5 minutes on shared stores.
A new writer holds every older manifest and its files for a full X from startup,
since their supersession times are unknown. Snapshot holds need no new metadata.

A previously active incarnation also keeps its tree for X after retirement.
The catalog records its retirement wall time after retirement is visible, so
the deadline survives restart and handoff; allow a margin for node clock skew.
Never-active reservations are removed immediately. The assigned node sweeps due
trees at startup and on the HTTP expiry tick. Without HTTP, pending trees wait
for the next startup.

## Reading from the store

Readers acquire the byte ranges they use (see held ranges below); on RAM and
mmap a hold is a plain view into the buffer or mapping, and FAISS IVF lists are
scanned in place. For step 4 that means:

- The cache is charged to the node's budgets and evicts: on local disk under
  the page cache, or in RAM charged to the memory budget on a node without
  disk.
- A reader uses only bytes that are local. A first implementation can fetch
  a segment's files whole before the segment opens. Later ones fetch just the
  ranges a query needs, as described below. Checking fetched bytes against their digests is optional, as
  `--store.verify-on-open` is for local files today.
- A segment's set of files does not depend on the storage backend.
- A required fetch that still fails after bounded retries within the request
  deadline fails the query with an error. A partial-results option would first
  need a definition of what counts, facets and ranking mean with segments
  missing.

Residency works at three levels:

- A collection can be closed: no reader, nothing local beyond its manifest.
  It opens on the first query that does not skip it, and closes again when
  idle. Many rolled-over collections, most of them cold, cost little until
  they are searched.
- A segment can be skipped from its catalog, or cached whole.
- A segment can be partly resident, with only the byte ranges that queries
  have used held locally.

The default unit of caching is the whole segment, fetched through the same
install path followers use, so readers do not change. It suits queries that
touch most of a segment. Range reads are also planned, for large segments
under selective queries. A large collection keeps its segment count low
because every segment adds per-query cost, so its segments are large and
mostly partly resident.

Range reads fit the segment format. A reader does not chase arbitrary
pointers through a file. It reaches data through a fixed set of per-field
regions recorded in the segment catalog (term index, terms, doc postings,
positions, column, points, norms), and within a region through offsets it
has just decoded. Those are the points where a reader fetches and pins a
range before using it:

- IVF lists are already acquired and released per list during a scan.
- A term's postings slice is known once the term dictionary seek returns.
- A column or points region can be fetched whole for its field, or by block
  from the region's metadata.

Fetching at those points, rather than on a page fault, lets a query collect
the ranges it needs first, merge nearby ones, and issue them together.
Object stores charge per request and each request has high latency, so
faults served one at a time would be both slow and expensive.

### Held ranges

`InputFile::acquire({offset, length})` checks the range against the file size
and returns a move-only `HeldRange`. The bytes stay valid until the hold is
released, and a hold must be released before its `InputFile`. RAM and mmap
return null-owner views valid for the file's lifetime and report `holdsFree()`;
other backends release through the hold's `HoldOwner`.

- A hold also makes readable the next 16 bytes of the file, as far as the file
  extends, with unspecified contents, because SIMD decoders over-read by up to
  that much. The format guarantees those bytes exist wherever a decoder reads
  past a part: the writer pads every data-only file by 16 bytes, and the
  catalog follows the data in file 0.
- The data address is congruent to the file offset modulo 16, so alignment the
  writer gave a structure in the file holds in memory.

`InputFile::acquireBatch(ranges, done)` acquires a batch of ranges together,
so a backend can merge them and issue them at once. It never throws: it
calls `done` exactly once, possibly inline, from any thread, with one hold
per range in the order requested or with an error and no holds, and every
failure, including one to issue the batch, arrives that way. The holds
follow the single-acquire contract. The default acquires each range in turn
and completes inline, and `acquireAll(ranges)` blocks on the batch and
returns its holds.

A completion runs on a backend thread and takes the locks of whatever keeps
the holds, such as a segment's holds lock. A caller must not hold, across a
backend call, any lock that a batch completion takes: a backend that serves
blocking acquires and completes batches on the same threads would deadlock.
`SegmentCore::retain()` therefore acquires outside its holds lock.

A segment reads every part through `SegmentCore::slice()`. Its
`acquireSlices()` acquires parts ahead in one batch per file, skipping parts
already held, and keeps the holds the same way. A manifest open acquires
each file's header, the catalog range, the version runs and, for segments
with nested spaces, the ownership ends and owners columns; never a whole
segment file. It opens every file the manifest lists, then issues one batch
per file: file 0's header with the catalog, every other file's header
alone. A snapshot open issues the batches of all its new segments before it
waits on any, so a backend that completes batches asynchronously serves
every header and catalog in one round. A header's hold is released once
checked. The version runs and a nested segment's ownership columns are
still acquired after that round, one blocking acquire at a time per
segment. Each segment's batches report failure to that segment, and the
segments fail in order as if opened one at a time: the lowest failing
segment's error surfaces, naming the segment, whichever batch completed
first. The open takes the same path on every backend; files with free
holds (RAM, mmap) differ only in that `slice()` retains nothing for them
and `acquireSlices()` issues no batch. For now the segment keeps the holds
it hands out until it closes, so nothing held is evicted; releasing holds
when a request ends comes with a bounded cache.

- A manifest open passes each file's recorded size to `Directory::openFile`
  and checks it against `size()` once per file; after a sized open, `size()`
  makes no request. A remote directory can keep a cached listing of its
  prefix (names, sizes, ETags), answer `openFile` and `size()` from it, and
  list again when a name is missing, so neither existence nor size costs a
  request per file. With a passed size it needs the listing only for
  existence.
- Each acquire carries a `ReadClass`. `METADATA` marks what a reader navigates
  by: file headers, the catalog, version runs, term block offsets and tries,
  the stored-field chunk index, and the directories and indexes over column
  and point blocks. Everything else, term blocks included, is `DATA`.
  Whoever keeps a hold decides how long it lives: the segment for what it
  keeps, a request for what it uses. The class is a hint a cache or backend
  may use for residency and scheduling after release, and it never changes
  the bytes returned. A part has one class whichever reader acquires it.

The manifest identifies the segment catalog at the end of file 0 by offset,
length and xxh3, and rejects a segment without that range. Every open from
the manifest checks the catalog bytes against the hash before decoding them,
whether the bytes are mapped locally or fetched as a range, so a damaged
catalog fails the open as a segment corruption error instead of decoding
wrong offsets. A catalog found from file 0's trailer alone has no manifest
hash and is not checked. The catalog holds a field table for each row space, then
the directory and its length. A field table contains name-sorted records
and their offsets. Each record contains a header followed by the parts
present in its part mask. A separately placed extent has an absolute file
number and offset. Adjacent pieces share that location and carry lengths
where the reader cannot derive them from the record and row-space counts.

The parts are the term dictionary and its postings, one column, points,
norms, a stored-fields resource, a docs bitmap, end value ranks, vector
owners, and the id filter. The id filter is a part of the root `id` field
only: a split-block Bloom filter over the dictionary's terms, about 10 bits
per term in 32-byte blocks, sized for the segment's document count. Its keys
are the XXH3 of each term as the dictionary stores it (the fitted form, so a
long id is never falsely absent), the same hash whose low byte the term
blocks keep. Every flush and merge rebuilds it from the terms it writes, so
it has exactly one key per term. The writer checks an id against the exact
dictionary bounds in the record header and then the filter, and looks up
only the segments and ids that pass. About 1% of absent ids pass.

Each field record encodes the schema flags as an unsigned vint, excluding
`ABSTRACT` and `DERIVED`. Bits 0-2 encode the cumulative postings level
(0, 1, 3 or 7). Bits 3-7 are `COLUMN_STORED`, `MULTI_VALUED`, `STORED`,
`INDEX_RANGE` and `MULTI_ARRAY`. Bits 8 and 9 are `NUM_TOKENS_APPROX` and
`NUM_TOKENS_EXACT`. Intervals set the multi-valued and array bits for their
physical endpoint pairs. The state byte carries the segment-specific
`MAY_HAVE_FITTED` flag and records the presence of bounds and an explicit
column value count. The field type determines the bounds kind.

The first step-4 work is a prototype of range reads against MinIO or S3. It
should show that eviction stays bounded with more data than the cache holds
and that fetch failures propagate correctly. Its measurements set chunk
sizes, prefetching and cache policy. They should count both the requests a
query makes and the rounds of requests it waits on in sequence, since each
read today depends on the one before it: the catalog, the term index, term
blocks, then postings.

Merging requests reduces their number but not the rounds, so the index
format can change too. A later layout could let a reader know, from
metadata it already holds, where all of a query's data lies, and gather it
in one pass where that pays: for example, small postings stored with their
term entries, or a term block placed next to its terms' postings. Today a
field's postings are spread across all of a segment's files.

### Lazy segment opening

Opening a snapshot builds and holds a reader for every segment, and a mapped
file keeps its disk space until its reader goes away. Both pruning and
eviction therefore need segments to open lazily. A segment starts with only
its catalog loaded and its data files closed. Planning and pruning work
from the catalog, and a segment opens fully only when a query needs its
data.

### A writer over cold data (optional)

Rolling over to new collections keeps the collection a writer is writing
fully local, so most uses never need a writer over data it does not hold.
That is needed only for one very large collection that keeps taking writes
(see the open questions). A writer whose older segments are not local also
needs:

- a merge policy that leaves old, large segments alone, or merges that fetch
  their inputs through the cache;
- id lookups for updates and deletes that skip cold segments without
  fetching them. Append-only collections need no lookups. Otherwise the
  exact id bounds in each segment's catalog and its id filter part do the
  skipping, from the catalog alone;
- the syncer reporting which files are published, so that eviction knows
  what it may drop.

## Pruning metadata

Field record headers in the catalog hold per-segment bounds, counts, and
type information. The bounds include encoded
numeric extrema, dictionary endpoints, and geo and interval extrema.
Dictionary endpoints can be rounded to a prefix lower bound or an
exclusive prefix successor upper bound. Each endpoint uses one length byte
followed by its bytes.

A planner can use the header without touching column or dictionary data.
On a cold node the manifest's `SegmentInfo` identifies the catalog bytes
for a range read. The catalog directory points to the separate version runs.
Per-field bounds stay in the catalog so manifests do not repeat them for
every segment.

A collection-level summary in each manifest is planned: per-field bounds
over all of its segments. It would be per field, not per segment, so it stays
small, and it would change only when the set of segments does. A search
across many collections could then skip a whole collection from the manifest
alone, without reading any segment's catalog or opening the collection.

Per-segment bounds prove "no match" when a query's range does not overlap
them, and "every document matches" when the minimum and maximum equal the
queried value and every document has the field. Declared partition
descriptors, which would reject documents at ingest, would add a
collection-level guarantee about future documents. They wait for a concrete
use.

## Many collections

Searching several collections is a direction, not scheduled. The likely shape
is one search over the segments of each collection's pinned snapshot, so that
existing execution merges results across segments. Pruning then works at both
levels: a collection's summary skips it entirely, and segment bounds skip
segments within the collections that remain. Open problems:

- segment identity across collections, which the filter cache keys on;
- per-reader ordinal maps, which would rebuild whenever any member commits;
- schema compatibility, including analysis.

Scoring uses term statistics from every segment in the search by default.
Statistics from only the segments or collections that can match will be an
option, which keeps a cold query's reads within the segments it did not
prune.

## Open questions

- Range-read chunk sizes, cache policy and writer layout, after the step-4
  prototype.
- One very large collection over a long time span, as an alternative to
  rolling over to new collections. This needs cheap retention (deleting
  whole segments when a time predicate covers all of their documents, which
  per-segment bounds can prove), a merge policy that keeps segments' time
  ranges tight while holding the segment count to around a hundred, partly
  resident segments, since those segments are large, and a writer over cold
  data.
- Shards within a collection. Today a collection is stored as one shard
  holding one index, and the shard level is a placeholder that appears in the
  stats and cache-control APIs. Splitting a collection into several shards
  would need an atomic commit across them, a HEAD, syncer and lease per
  shard, routing of updates by id, and per-shard scoring statistics. The
  alternative is several collections behind one name: writes routed by a key
  such as time, searches over the group as above, retention by dropping a
  member, and later placement of members on different nodes.
  It may be better to just drop the shard concept...
- The S3 client: an SDK, or a small client on the existing HTTP stack.
- Whether RAM-backed writers wait for remote durability by default.
