# Vector Search

Solux stores vector fields in the normal column store and can also build
per-segment ANN (FAISS IVF+PQ) aux indexes for searchable vector fields. The
column is always the source of truth. Vector search supports exact flat KNN,
doc collapse for multi-valued vector fields, liveDocs filtering, and
full-precision rescoring.

This page is the user-visible contract: how ANN indexes get built, the query
knobs, score semantics, and recall. For how it works inside the engine, see
[design/vector-search.md](../design/vector-search.md).

## Indexing

ANN aux indexes are built at commit time, when `build_aux_indexes` on the
commit selects a vector field or `"*"`. Plain commits without vector selectors
never build them. Once a segment has a vector aux index, it follows the
segment: later commits carry it forward, and delete-only commits keep it
(deleted documents' vectors are filtered out at query time).

A commit whose aux build fails has no side effects: nothing partial is
published, and the caller sees the error and decides whether to retry.

Whether a segment gets an ANN index is size based. IVF+PQ is built only when
the segment has enough vectors to train on and enough scan cost to justify an
ANN index; segments below either threshold stay on the exact column-scan path.
This mixed composition is normal: a single query can use ANN for large
segments and exact scan for small ones.

Merges carry the choice forward: a merged segment gets fresh aux indexes for
the vector fields being built, when it passes the same size thresholds. If a
merge-time build fails, the merged segment is published without the aux index
and that field is served by exact scan until a later explicit build succeeds.

Rebuilding with new options does not require reindexing documents: drop the
segment's aux index entry and run a selected commit to rebuild from the stored
vector column.

## Querying

Each segment is searched with its ANN index when it has one, or by exact scan
of the vector column when it does not.

`nprobe` is a merge-stable IVF effort knob. The wire name stays familiar, but
the value is interpreted as the number of lists Solux would probe if the field
were a single IVF index built with `nlist = sqrt(live_vector_count)`, capped
the same way as the builder's `nlist`. That total effort is spread across the
current per-segment indexes, so pure merges do not change the requested
effort. When `nprobe` is `0`, Solux chooses an adaptive default and may
auto-deepen breadth when filters or doc collapse leave too few live documents.
An explicit `nprobe` pins the total effort cap.

Probed lists are always scanned in full; `nprobe` (which selects lists by
relevance) is the only work limiter.

`min_scan_fraction` optionally sets a direct floor on the fraction of live
vector values scanned. It is a float in `[0,1]`; values like `0.001` mean 0.1%
of the live vector values, not 0.001%.

`refine_candidates` controls overfetch for approximate ANN: how many
approximate candidates are collected before the full-precision rescore. An
explicit value pins the candidate pool to exactly that many approximate
candidates (clamped up to `k`) - an absolute count, so large-`k` callers are
not forced to choose between coarse multiplier steps. When unset, the default
pool is affine in `k`, with a multiplier that decays from ~10 at `k=1` to a
small floor by `k=1000`. Quantization mis-ranking displaces a true neighbor by
a roughly constant number of candidates regardless of `k`, so small `k` needs
the fixed headroom; large-`k` requests are recall-oriented retrieval whose
near-tied tail does not benefit from extra overfetch, so the pool stays
proportionate instead of exploding. The final candidate pool is always
rescored from the full-precision column, sorted by exact score, and collapsed
to one hit per document.

`exact` requires exact (true top-k) results. It is a result contract, not an
execution mode: the engine uses a path that guarantees exactness - currently
an exhaustive scan over the stored vector column - and does not consult
approximate ANN indexes. `nprobe` and `refine_candidates` are ignored. Cost is
linear in the number of stored vectors. Query semantics are otherwise
identical to the default path (same filters, same multi-valued collapse, same
score scale), which makes `exact` the ground truth for measuring ANN recall:
run the same query twice, once with `exact`, and compare.

Parallel execution never changes results: a query run in parallel mode returns
bit-identical results to a serial run of the same query.

## Scoring

L2 scores are reported as `1 / (1 + squared_distance)`. Inner product scores
are the dot product. Cosine is implemented as inner product over normalized
vectors.

Cosine fields normalize on write by default (`normalize_on_write=true`), so
reading the vector column back returns the unit vector, not necessarily the
originally submitted bytes. Zero and near-zero cosine vectors are skipped at
index time with a warning, since they have no direction. If a cosine field
sets `normalize_on_write=false`, the stored column keeps the raw submitted
vectors (exact retrieval) and candidate rescoring normalizes the stored vector
at query time instead.

## Recall Semantics

IVF+PQ is approximate. Higher `nprobe` and higher `refine_candidates`
generally increase recall and latency. Setting `nprobe` to at least the
reference `sqrt(live_vector_count)` list count, or setting
`min_scan_fraction=1`, makes the ANN search exhaustive over IVF lists, but PQ
ordering still decides which candidates are returned before column rescore.
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
