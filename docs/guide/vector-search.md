# Vector Search

Luxir stores vector fields in the normal column store and can also build
per-segment ANN (FAISS IVF+PQ) aux indexes for searchable vector fields. The
column is always the source of truth. Vector search supports exact flat KNN,
doc collapse for multi-valued vector fields, liveDocs filtering, and
full-precision rescoring.

This page is the user-visible contract: how ANN indexes get built, the query
knobs, score semantics, and recall. For how it works inside the engine, see
[design/vector-search.md](../design/vector-search.md).

## Define a vector field

The default `_v` suffix recognizes a vector value, but it is storage-only until
a similarity metric is selected. Define dimensions and a metric explicitly for
a searchable field:

```bash
curl http://localhost:9400/collections/books/_schema -d '{
  "fields": {
    "embedding_v": {
      "type": "vector",
      "dims": 3,
      "metric": "cosine"
    }
  }
}'
```

Supported metrics are `l2`, `ip` (inner product), and `cosine`. A positive
`dims` rejects wrong-sized values at ingest; when dimensions are omitted, the
first vector in each segment establishes them. Explicit dimensions are easier
to operate because a bad producer fails immediately against a collection-wide
contract.

## Index vectors through typed gRPC values

Document vectors currently require the typed protobuf `Val.vec` arm. A bare
array inside an HTTP document is decoded as a generic numeric array and is not
promoted to `Vector`, so HTTP vector ingest is not implemented yet. kNN query
vectors over HTTP do work because `KnnQuery.query` has a statically known vector
type.

The protobuf text shape of a row update is:

```proto
collection { name: "books" }
docs {
  fields { key: "id" value { s: "b1" } }
  fields { key: "title_w" value { s: "dune" } }
  fields { key: "kind_s" value { s: "fiction" } }
  fields {
    key: "embedding_v"
    value { vec { f32 { v: 0.8 v: 0.1 v: 0.1 } } }
  }
}
commit {}
```

Generated clients construct the same `UpdateRequest` and send it through
`Indexer.Update` or `Indexer.UpdateStream`. Set `multi: true` for several
vectors per document and populate `Val.arr_vec` with several `Vector.f32`
values. Search collapses them to one hit per document using that document's
best similarity.

## Query vectors

```http
POST /collections/books/_search

{
  "query": {
    "knn": {
      "field":"embedding_v",
      "query":[0.75,0.15,0.10],
      "k":20
    }
  },
  "limit":10,
  "get_scores":true,
  "fields":["id","title_w"]
}
```

`k` is the number of nearest-neighbor documents produced by the query node;
the surrounding `top_docs.limit` controls how many are returned. With no ANN
overlay, or with `exact: true`, the engine scans the full-precision vector
column. Once an ANN overlay exists, the default path may use it segment by
segment and then rescore candidates from the full-precision column.

## Filter inside kNN

Put ordinary filters on the same `top_docs` operation:

```json
{
  "query": {
    "knn": {
      "field":"embedding_v",
      "query":[0.75,0.15,0.10],
      "k":20
    }
  },
  "filter":["kind_s:fiction"],
  "limit":20,
  "fields":["id","title_w"]
}
```

The filter is part of vector candidate search, not a post-pass over an
unfiltered top 20. When enough matching documents exist and the ANN search can
reach them within its effort cap, `k: 20` means 20 filtered neighbors rather
than 20 minus whatever a later filter discarded. Adaptive `nprobe` can deepen
automatically when filters or multi-value collapse underfill the result.

## Hybrid search with RRF

Reciprocal rank fusion combines independently ranked sources without forcing
BM25 and vector similarity onto an invented common score scale:

```http
POST /collections/books/_search

{
  "ops": {
    "hybrid": {
      "fusion": {
        "sources": {
          "lexical": {
            "query": {"match":{"title_w":"dune"}},
            "limit": 100
          },
          "semantic": {
            "query": {
              "knn": {
                "field":"embedding_v",
                "query":[0.75,0.15,0.10],
                "k":100
              }
            },
            "limit": 100
          }
        },
        "filter": ["kind_s:fiction"],
        "rrf": {"k":60},
        "limit": 10,
        "get_scores": true,
        "fields": ["id","title_w"]
      }
    }
  }
}
```

For document `d`, RRF computes `sum(1 / (k + rank))` over sources containing
`d`, with ranks starting at one. The fusion-level filter is shared by every
source and computed once per segment. A source may add its own `filter`; the
shared and source-specific filters are ANDed. Source `query`, `filter`, `sorts`,
and `limit` define its ranking; response-shape fields belong on the fusion.
Fusion-level sub-ops are not implemented, so its `ops` map must be empty.

RRF source limits are candidate-pool decisions. A document outside a source's
limit cannot contribute from that source, so choose pools large enough for the
recall required by the final fused `limit`.

## Build ANN overlays

ANN aux indexes are built at commit time, when `build_aux_indexes` on the
commit selects a vector field or `"*"`. Plain commits without vector selectors
never build them. Once a segment has a vector aux index, it follows the
segment: later commits carry it forward, and delete-only commits keep it
(deleted documents' vectors are filtered out at query time).

Build every eligible missing vector overlay while committing:

```json
{"commit":{"build_aux_indexes":["*"]}}
```

Or select one overlay by its `vec.` name:

```json
{"commit":{"build_aux_indexes":["vec.embedding_v"]}}
```

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

There is no public command to remove or rebuild an overlay that already exists
on a segment. New and merged segments build from their full-precision vector
columns; an existing segment without an overlay can receive one through a later
selected commit.

## Querying

Each segment is searched with its ANN index when it has one, or by exact scan
of the vector column when it does not.

`nprobe` is a merge-stable IVF effort knob. The wire name stays familiar, but
the value is interpreted as the number of lists Luxir would probe if the field
were a single IVF index built with `nlist = sqrt(live_vector_count)`, capped
the same way as the builder's `nlist`. That total effort is spread across the
current per-segment indexes, so pure merges do not change the requested
effort. When `nprobe` is `0`, Luxir chooses an adaptive default and may
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
index time, since they have no direction; this is currently reported only as a
server-log warning, not in the update response. If a cosine field
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
  set, and no automatic exact fallback when a selective filter still starves
  the ANN search after breadth deepening. Use `exact: true` when exact filtered
  top-k is the required contract.
