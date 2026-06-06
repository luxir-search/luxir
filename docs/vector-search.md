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
selects a vector field or `"*"`. After a segment has a vector overlay, that
overlay follows the segment by liveness: later commits carry it forward without
checking the shard core generation. New above-threshold segments that lack an
overlay for an already-built vector field get their own overlay during the
commit pipeline. Delete-only commits keep existing overlays; deleted-document
vectors remain in FAISS and are filtered at query time.

Build policy is per segment and size based. IVF+PQ is attempted only when the
segment has enough vectors to satisfy the training floor and enough
`N * dims` scan cost to justify an ANN index. Segments below either threshold
stay on the flat-over-column path. This mixed composition is normal: a query
can use FAISS for large segments and exact column scan for small segments.

The IVF+PQ training set is a strided sample across the segment's vector values,
not a prefix sample. This avoids training only on the oldest values when data
inside a segment is time ordered. The builder normalizes vectors for FAISS when
the field uses cosine and the stored column is raw.

Merges drop overlays for merged-away segments. The merged segment is treated as
a new segment and gets a fresh overlay during the merge's commit when it passes
the same thresholds. Rebuilding with new options does not require reindexing
documents: drop the segment overlay entry and run a selected commit to rebuild
from the stored vector column.

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

`nprobe` currently applies per segment and is clamped to each segment's list
count. When `nprobe` is `0`, each segment starts from its own default breadth.
Breadth auto-deepening can widen segments when filters or doc collapse leave
too few live documents. This is an interim per-segment policy; a future total
effort knob will allocate breadth globally.

Probed lists are always scanned in full. Entries within an IVF list are stored
in insertion order, not relevance order, so a partial list scan would drop
arbitrary candidates; `nprobe` (which selects lists by relevance) is the only
work limiter.

`refine_candidates` controls overfetch for approximate ANN. An explicit
value pins the candidate pool to exactly that many approximate candidates
(clamped up to `k`) - an absolute count, like `nprobe`, so large-`k` callers
are not forced to choose between coarse multiplier steps. When unset, the
default pool is affine with a multiplier that shrinks as `k` grows - a fixed count plus
`k` times a ratio that decays from ~10 at `k=1` to a small floor by
`k=1000`. Quantization mis-ranking displaces a true neighbor by a roughly
constant number of candidates regardless of `k`, so small `k` needs the
fixed headroom; large `k` requests are recall-oriented retrieval whose
near-tied tail does not benefit from extra overfetch, so the pool stays
proportionate instead of exploding. Candidates are unioned across deepen
rounds, rescanned from the full-precision column, sorted by exact score, and
collapsed to one hit per document.

With per-segment indexes, candidate depth is distributed by segment size for
now: each segment receives a proportional share of the requested candidate
pool, floored at `min(k, segment_vector_count)`. A future allocator will own
global candidate and breadth budgeting.

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
increase recall and latency. Setting `nprobe` to the full `nlist` and using a
large enough candidate pool makes the aux search exhaustive over IVF lists, but
PQ ordering still decides which candidates are returned before column rescore.
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
- FAISS indexes are decoded into memory on first use. The vector column itself
  is mmapped.
- Per-segment `nprobe` and candidate distribution are interim policies. They
  preserve the current API but do not pin total work across different segment
  counts.
