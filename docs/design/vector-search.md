# Vector Search Internals

Design of the vector indexing and search path: how per-segment FAISS aux
indexes are recorded, built, and queried. The user-visible contract (query
knobs, scoring, recall) is in
[guide/vector-search.md](../guide/vector-search.md).

The stored vector column is the source of truth; ANN indexes are derived,
per-segment aux overlays behind an engine seam. Exact flat KNN, cosine
raw-column normalization, full-precision rescoring, and rebuilds all read or
regenerate from the column.

## Overlay Lifecycle

Vector aux indexes are recorded as segment overlays in the index-level
`s.olux` file. Each segment's `SegmentInfo` can carry `AuxIndexInfo` entries
whose vector kind is `vector_faiss` and whose opaque metadata is
`VectorAuxMeta`.

A vector aux build is introduced during commit when `build_aux_indexes`
selects a vector field or `"*"`. Plain commits without vector selectors never
build vector overlays. After a segment has a vector overlay, that overlay
follows the segment by liveness: later commits carry it forward without
checking the shard core generation. Delete-only commits keep existing
overlays; deleted-document vectors remain in FAISS and are filtered at query
time.

Explicit vector selectors also activate those concrete overlay names in the
writer. That activation is process-local unless it produced a durable overlay
entry. A commit that fails mid-build has no side effects: staged overlay files
are deleted, no overlay entries are published, and no names are activated -
the caller sees the error and decides whether to retry. Startup seeds active
names only from overlays already present in `s.olux`, so Solux never treats
intent-only state as a boot-time "must build" queue.

Explicit commit-time vector builds stage overlay files and entries across the
whole selected commit. If any requested vector build fails, staged overlay
files are deleted and no partial vector overlay entries are retained for a
later plain commit to publish.

Rebuilding with new options does not require reindexing documents: drop the
segment overlay entry and run a selected commit to rebuild from the stored
vector column.

## Build Policy

Build policy is per segment and size based. IVF+PQ is attempted only when the
segment has enough vectors to satisfy the training floor and enough
`N * dims` scan cost to justify an ANN index. Segments below either threshold
stay on the flat-over-column path. This mixed composition is normal: a query
can use FAISS for large segments and exact column scan for small segments.

The IVF+PQ training set is a strided sample across the segment's vector
values, not a prefix sample. This avoids training only on the oldest values
when data inside a segment is time ordered. The builder normalizes vectors for
FAISS when the field uses cosine and the stored column is raw.

## Memory-Mapped Index Layout

Index data stays memory-mapped at query time. The aux file holds a small FAISS
header (index parameters, IVF centroids, PQ codebooks) followed by the
inverted-list payloads (PQ codes and vector ids) in Solux's own layout; the
reader decodes only the header into memory and serves list scans directly from
the mmapped file. Residency of the bulk index data is therefore managed by the
OS page cache, like the vector column itself, rather than forced into process
RAM.

## Merging

Merges drop overlays for merged-away segments. The merged segment is treated
as a new segment and gets fresh overlays inside the merge-private phase for
active vector fields that pass the same thresholds. Whichever later commit
publishes the merged segment publishes its overlay entries atomically with the
segment. If a merge-time vector overlay build fails, Solux deletes the staged
overlay files, publishes the merged segment without vector overlays, and
serves that field through the exact flat column fallback until a later
explicit build succeeds.

## Query Execution

`KnnQuery` creates one vector engine per segment. A segment uses its
`vec.<field>` FAISS overlay when present, or scans the vector column exactly
when no overlay exists. FAISS ids are segment-local vector value ranks, so the
query path maps each returned value rank back through that segment's vector
column metadata before liveDocs, filters, scoring, and multi-valued collapse.

IndexReader opens and pins overlay files eagerly when it opens an index
version, so cleanup can unlink old files without breaking existing readers.
The FAISS index bytes are decoded lazily on first kNN use and then cached on
the segment reader.

Internally, an `nprobe` request becomes a scan fraction
`nprobe / sqrt(live_vector_count)` applied across the current per-segment
indexes; that is what makes the knob merge-stable.

For per-segment IVF, Solux ranks all segments' IVF lists by query-to-centroid
distance, then probes the globally best lists until the requested scan
fraction is reached. List cost is based on that list's live vector count
divided by the field's live vector count. This handles uneven list sizes and
avoids applying equal work to every segment. For an IVF segment with deletions
the live count is exact: it rides along with the segment's cached
rank-liveness bitmap, which is rebuilt per delete generation in time
proportional to the deleted docs (for a dense single-valued field the bitmap
simply borrows the liveDocs bits). Flat-scanned segments with deletions use a
cheap upper bound instead (exact counting there would scan the field per
query); the bound only errs toward slightly leaner initial effort, which
auto-deepening recovers.

Probed lists are always scanned in full. Entries within an IVF list are stored
in insertion order, not relevance order, so a partial list scan would drop
arbitrary candidates; `nprobe` (which selects lists by relevance) is the only
work limiter.

Candidates are unioned across deepen rounds using approximate scores for the
widen decision. From the first deepen round on, already-pooled vectors are
excluded from every engine's scan: the pooled set is folded into per-query
copies of the segments' liveness bitmaps (all-ones where a segment has no
deletions) and handed to the scanners as an eligibility filter, so each
round's result heap is spent entirely on new candidates and each round
requests only the projected shortfall rather than re-requesting the whole
pool. The shared per-segment liveness cache itself is never modified by
queries. Once widening finishes, the final candidate pool is rescanned from
the full-precision column, sorted by exact score, and collapsed to one hit per
document.

## Parallel Execution

A single query's scan and rescore run in parallel when the request executes in
parallel mode. The selected IVF lists are split into contiguous per-segment
chunks sized by their actual vector counts, and each chunk (plus each
column-scanned segment) is scanned as an independent task; the terminal
full-precision rescore runs one task per segment whose candidate bucket
exceeds a small grain (smaller buckets, including the common small-`k`
single-segment pool, fold inline on the calling thread). Task boundaries do
not depend on the execution mode, and every bounded candidate cut uses a total
order, so a parallel run returns bit-identical results to a serial run of the
same query.

## What This Means for Users

- Bulk ANN index data is never forced into process RAM: residency is managed
  by the OS page cache, so memory use stays predictable and indexes larger
  than RAM degrade gracefully instead of failing.
- Because the column is the source of truth, ANN indexes can be rebuilt or
  retuned per segment without reindexing any documents.
- Small segments are served exactly rather than through a poorly trained ANN
  index, and a failed build never leaves partial state - the fallback is
  always exact search over the column.
- Parallelism is purely a speed knob: results are bit-identical to serial
  execution.
