# Vector Search

Solux stores vector fields in the normal column store and can also build one
FAISS aux index per searchable vector field. The column is always the source of
truth. It supports exact flat KNN, doc collapse for multi-valued vector fields,
liveDocs filtering, and full-precision rescoring.

## Indexing

Vector aux indexes are built during commit when `build_aux_indexes` selects a
vector field or `"*"`. A normal aux build attempts IVF+PQ when the field has
enough vectors to train the coarse centroids and PQ codebooks. Small fields stay
on the flat-over-column path because exact scan is cheap and low-sample PQ
training is poor.

The IVF+PQ training set is a strided sample across the shard's vector values,
not a prefix sample. This avoids training only on the oldest segment data when
the corpus is time ordered. The builder normalizes vectors for FAISS when the
field uses cosine and the stored column is raw.

Aux indexes are invalidated when the segment composition changes. The current
MVP rebuilds the whole shard-level aux index on the next selected commit.

## Querying

`KnnQuery` uses the aux engine when a matching `vec.<field>` aux reader is
available. Otherwise it scans the vector column exactly.

`nprobe` sets the exact number of IVF lists probed (clamped to the index's
list count) - every search round probes exactly that many, no more and no
fewer. It is a latency control: an explicit `nprobe` combined with a
selective filter can return fewer than `k` documents. Breadth auto-deepening
applies only when `nprobe` is `0`: the search starts at the index default and
the host widens it when filters or doc collapse leave too few live documents.

Probed lists are always scanned in full. Entries within an IVF list are stored
in insertion order, not relevance order, so a partial list scan would drop
arbitrary candidates; `nprobe` (which selects lists by relevance) is the only
work limiter.

`refine_factor` controls overfetch for approximate ANN. The query asks the aux
engine for roughly `k * refine_factor` vector candidates, unions candidates
across deepen rounds, rescans approximate hits from the full-precision column,
sorts by exact score, and then collapses to one hit per document.

`exact` requires exact (true top-k) results. It is a result contract, not an
execution mode: the engine uses a path that guarantees exactness - currently
an exhaustive scan over the stored vector column - and does not consult
approximate ANN indexes. `nprobe` and `refine_factor` are ignored. Cost is
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

IVF+PQ is approximate. Higher `nprobe` and higher `refine_factor` generally
increase recall and latency. Setting `nprobe` to the full `nlist` and using a
large enough `refine_factor` makes the aux search exhaustive over IVF lists, but
PQ ordering still only decides which candidates are returned before column
rescore. For exact results, rely on the flat-over-column path.

Similarity scores are model-relative: the score distribution depends on the
embedding model and corpus, so thresholds and "good score" intuitions must be
calibrated per deployment, not carried between models.

## Current Limitations

- The ANN index is shard-level and is fully rebuilt when segment composition
  changes (a commit that adds or merges segments rebuilds the index).
  Per-segment indexes with incremental reuse are planned.
- IVF+PQ is the only ANN index type. No HNSW yet.
- Filters are always applied inside the vector search. There is no cost-based
  choice yet between filtered ANN search and exact search over the filtered
  set, and no automatic exact fallback when a selective filter starves the
  ANN search.
- The ANN index is loaded fully into memory at open. The vector column itself
  is mmapped.
- Default IVF tuning parameters (nlist, nprobe, refine) have not yet been
  validated against recall benchmarks; treat them as reasonable starting
  points, not tuned guarantees.
