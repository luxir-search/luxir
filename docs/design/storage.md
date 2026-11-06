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

A node that reads a snapshot from the store holds a lease on it, with a
bounded lifetime that the node renews while it uses the snapshot. Garbage
collection removes an object only when neither HEAD nor a live lease
references it. A lapsed lease means the snapshot expired, as it does for a
follower's pin today.

## Reading from the store

Readers work directly on mapped files or owned buffers, and FAISS IVF lists
are scanned in place. For step 4 that means:

- The cache is charged to the node's budgets and evicts: on local disk under
  the page cache, or in RAM charged to the memory budget on a node without
  disk.
- A reader uses only bytes that are local. The first implementation fetches
  a segment's files whole before the segment opens, so readers do not
  change. Later ones fetch just the ranges a query needs, as described
  below. Checking fetched bytes against their digests is optional, as
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
regions recorded in the field catalog (term index, terms, doc postings,
positions, column, points, norms), and within a region through offsets it
has just decoded. Those are the points where a reader fetches and pins a
range before using it:

- IVF lists are already acquired and released per list during a scan.
- A term's postings extent is known once the term dictionary seek returns.
- A column or points region can be fetched whole for its field, or by block
  from the region's metadata.

Fetching at those points, rather than on a page fault, lets a query collect
the ranges it needs first, merge nearby ones, and issue them together.
Object stores charge per request and each request has high latency, so
faults served one at a time would be both slow and expensive.

The catalog records where each region starts, but for most regions not its
length, so range reads may add lengths.

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

Opening a snapshot secures its files by holding a handle on each, and
reads only the manifest and the segment summaries. A segment's data files
are mapped and decoded (materialized) only when a search keeps the segment,
at one batched point after planning; see
[segment summaries](segment-summaries.md#lazy-opening). A held or mapped
file keeps its disk space until its reader goes away. On an object store the
handle becomes a lease on the snapshot's objects, which eviction must also
respect; a cold segment's catalog would then come from one range read.

### A writer over cold data (optional)

Rolling over to new collections keeps the collection a writer is writing
fully local, so most uses never need a writer over data it does not hold.
That is needed only for one very large collection that keeps taking writes
(see the open questions). A writer whose older segments are not local also
needs:

- a merge policy that leaves old, large segments alone, or merges that fetch
  their inputs through the cache;
- id lookups for updates and deletes that skip cold segments without
  fetching them. Append-only collections need no lookups. Otherwise
  per-segment id ranges, which the writer already keeps, and per-segment id
  filters do the skipping;
- the syncer reporting which files are published, so that eviction knows
  what it may drop.

## Pruning metadata

Per-segment bounds belong in the segment's field catalog (`SegFieldInfo`, at
the tail of the segment's first file), which already records term counts,
docs with the field, and value counts. Two bounds are added:

- numeric minimum and maximum, moved from the column's own metadata;
- first and last term for string and id fields, truncated so the bounds stay
  correct: a prefix for the lower bound, and the next value after a prefix
  for the upper bound.

On a node with the data, the bounds come with the catalog decode, and a
numeric range prunes a segment without touching column data. On a cold node,
`SegmentInfo` records the length of the first file's tail, so one range read
fetches the catalog, the segment info and the version runs together, with no
extra files. The bounds stay out of `SegmentInfo` itself, because it is copied
into every commit manifest.

Each manifest also carries a collection-level summary: per-field bounds over
all of its segments. It is per field, not per segment, so it stays small, and
it changes only when the set of segments does. A search across many
collections uses it to skip a whole collection from the manifest alone,
without reading any segment's catalog or opening the collection.

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
