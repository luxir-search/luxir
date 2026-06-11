# Vector Search

Solux stores vector fields in the normal column store and can also build
per-segment FAISS aux indexes for searchable vector fields. The column is
always the source of truth. It supports exact flat KNN, doc collapse for
multi-valued vector fields, liveDocs filtering, and full-precision rescoring.

## Indexing

Vector aux indexes are recorded as segment overlays in the index-level
`s.olux` file. Each segment's `SegmentInfo` can carry `AuxIndexInfo` entries
whose vector kind is `vector_faiss` and whose opaque metadata is
`VectorAuxMeta`.

A vector aux build is introduced during commit when `build_aux_indexes`
selects a vector field or `"*"`. Plain commits without vector selectors never
build vector overlays. After a segment has a vector overlay, that overlay
follows the segment by liveness: later commits carry it forward without
checking the shard core generation. Delete-only commits keep existing overlays;
deleted-document vectors remain in FAISS and are filtered at query time.

Explicit vector selectors also activate those concrete overlay names in the
writer. That activation is process-local unless it produced a durable overlay
entry. A commit that fails mid-build has no side effects: staged overlay files
are deleted, no overlay entries are published, and no names are activated -
the caller sees the error and decides whether to retry. Startup seeds active names only from overlays already present in
`s.olux`, so Solux never treats intent-only state as a boot-time "must build"
queue.

Build policy is per segment and size based. IVF+PQ is attempted only when the
segment has enough vectors to satisfy the training floor and enough
`N * dims` scan cost to justify an ANN index. Segments below either threshold
stay on the flat-over-column path. This mixed composition is normal: a query
can use FAISS for large segments and exact column scan for small segments.

The IVF+PQ training set is a strided sample across the segment's vector values,
not a prefix sample. This avoids training only on the oldest values when data
inside a segment is time ordered. The builder normalizes vectors for FAISS when
the field uses cosine and the stored column is raw.

Index data stays memory-mapped at query time. The aux file holds a small FAISS
header (index parameters, IVF centroids, PQ codebooks) followed by the
inverted-list payloads (PQ codes and vector ids) in Solux's own layout; the
reader decodes only the header into memory and serves list scans directly from
the mmapped file. Residency of the bulk index data is therefore managed by the
OS page cache, like the vector column itself, rather than forced into process
RAM.

Merges drop overlays for merged-away segments. The merged segment is treated as
a new segment and gets fresh overlays inside the merge-private phase for active
vector fields that pass the same thresholds. Whichever later commit publishes
the merged segment publishes its overlay entries atomically with the segment.
If a merge-time vector overlay build fails, Solux deletes the staged overlay
files, publishes the merged segment without vector overlays, and serves that
field through the exact flat column fallback until a later explicit build
succeeds.
Rebuilding with new options does not require reindexing documents: drop the
segment overlay entry and run a selected commit to rebuild from the stored
vector column.

Explicit commit-time vector builds stage overlay files and entries across the
whole selected commit. If any requested vector build fails, staged overlay files
are deleted and no partial vector overlay entries are retained for a later plain
commit to publish.

## Querying

`KnnQuery` creates one vector engine per segment. A segment uses its
`vec.<field>` FAISS overlay when present, or scans the vector column exactly
when no overlay exists. FAISS ids are segment-local vector value ranks, so the
query path maps each returned value rank back through that segment's vector
column metadata before liveDocs, filters, scoring, and multi-valued collapse.

IndexReader opens and pins overlay files eagerly when it opens an index
version, so cleanup can unlink old files without breaking existing readers.
The FAISS index bytes are decoded lazily on first kNN use and then cached on
the segment reader.

`nprobe` is a merge-stable IVF effort knob. The wire name stays familiar, but
the value is interpreted as the number of lists Solux would probe if the field
were a single IVF index built with `nlist = sqrt(live_vector_count)`, capped the
same way as the builder's `nlist`. Internally this becomes a scan fraction
`nprobe / sqrt(live_vector_count)`. That fraction is applied across the current
per-segment indexes, so pure merges do not change the requested total effort.
When `nprobe` is `0`, Solux chooses an adaptive default and may auto-deepen
breadth when filters or doc collapse leave too few live documents. Explicit
`nprobe` pins the total effort cap.

For per-segment IVF, Solux ranks all segments' IVF lists by query-to-centroid
distance, then probes the globally best lists until the requested scan fraction
is reached. List cost is based on that list's live vector count divided by the
field's live vector count. This handles uneven list sizes and avoids applying
equal work to every segment. For an IVF segment with deletions the live count
is exact: it rides along with the segment's cached rank-liveness bitmap, which
is rebuilt per delete generation in time proportional to the deleted docs (for
a dense single-valued field the bitmap simply borrows the liveDocs bits).
Flat-scanned segments with deletions use a cheap upper bound instead (exact
counting there would scan the field per query); the bound only errs toward
slightly leaner initial effort, which auto-deepening recovers.

`min_scan_fraction` optionally sets a direct floor on the internal scan
fraction. It is a float in `[0,1]`; values like `0.001` mean 0.1% of the live
vector values, not 0.001%.

Probed lists are always scanned in full. Entries within an IVF list are stored
in insertion order, not relevance order, so a partial list scan would drop
arbitrary candidates; `nprobe` (which selects lists by relevance) is the only
work limiter.

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

`refine_candidates` controls overfetch for approximate ANN. An explicit
value pins the candidate pool to exactly that many approximate candidates
(clamped up to `k`) - an absolute count, so large-`k` callers are not forced to
choose between coarse multiplier steps. When unset, the
default pool is affine with a multiplier that shrinks as `k` grows - a fixed count plus
`k` times a ratio that decays from ~10 at `k=1` to a small floor by
`k=1000`. Quantization mis-ranking displaces a true neighbor by a roughly
constant number of candidates regardless of `k`, so small `k` needs the
fixed headroom; large `k` requests are recall-oriented retrieval whose
near-tied tail does not benefit from extra overfetch, so the pool stays
proportionate instead of exploding. Candidates are unioned across deepen
rounds using approximate scores for the widen decision; from the first deepen
round on, already-pooled vectors are excluded from every engine's scan (the
pooled set is folded into per-query copies of the segments' liveness bitmaps -
all-ones where a segment has no deletions - and handed to the scanners as an
eligibility filter), so each round's result heap is spent entirely on new
candidates and each round requests only the projected shortfall rather than
re-requesting the whole pool. The shared per-segment liveness cache itself is
never modified by queries. Once widening finishes,
the final candidate pool is rescanned from the full-precision column, sorted by
exact score, and collapsed to one hit per document.

`exact` requires exact (true top-k) results. It is a result contract, not an
execution mode: the engine uses a path that guarantees exactness - currently
an exhaustive scan over the stored vector column - and does not consult
approximate ANN indexes. `nprobe` and `refine_candidates` are ignored. Cost is
linear in the number of stored vectors. Query semantics are otherwise
identical to the default path (same filters, same multi-valued collapse, same
score scale), which makes `exact` the ground truth for measuring ANN recall:
run the same query twice, once with `exact`, and compare.

## Scoring

L2 scores are reported as `1 / (1 + squared_distance)`. Inner product scores are
the FAISS dot product. Cosine is implemented as inner product over normalized
vectors.

Cosine fields normalize on write by default (`normalize_on_write=true`), so
reading the vector column back returns the unit vector, not necessarily the
originally submitted bytes. Zero and near-zero cosine vectors are skipped at
index time with a warning, since they have no direction. If a cosine field
sets `normalize_on_write=false`, the stored column keeps the raw submitted
vectors (exact retrieval) and candidate rescoring normalizes the stored vector
at query time instead.

## Recall Semantics

IVF+PQ is approximate. Higher `nprobe` and higher `refine_candidates` generally
increase recall and latency. Setting `nprobe` to at least the reference
`sqrt(live_vector_count)` list count, or setting `min_scan_fraction=1`, makes
the aux search exhaustive over IVF lists, but PQ ordering still decides which
candidates are returned before column rescore.
For exact results, use the `exact` flag.

Similarity scores are model-relative: the score distribution depends on the
embedding model and corpus, so thresholds and "good score" intuitions must be
calibrated per deployment, not carried between models.

## Current Limitations

- IVF+PQ is the only ANN index type. No HNSW yet.
- Filters are always applied inside the vector search. There is no cost-based
  choice yet between filtered ANN search and exact search over the filtered
  set, and no automatic exact fallback when a selective filter starves the
  ANN search.
