# Vector Search

A kNN query is a query like any other. It goes wherever a text query goes:
at the root of a request, under a boolean clause, inside a facet bucket, or
as one source of a hybrid ranking. It takes the same filters, and they apply
*inside* the vector search rather than as a pass over an unfiltered top-k.
Vectors are stored in the index beside the documents, so there is no
separate vector store to run or keep in sync.

## Search by vector

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
  "fields":["id","title_t"]
}
```

```json
{
  "docs": [
    {
      "id": "b1",
      "title_t": "Dune",
      "_score_": 0.99735457
    },
    {
      "id": "b2",
      "title_t": "Dune Messiah",
      "_score_": 0.9652815
    },
    {
      "id": "b3",
      "title_t": "The Making of the Atomic Bomb",
      "_score_": 0.30873275
    }
  ]
}
```

`k` is how many nearest documents the query node produces; `limit` is how
many the request returns. The score is the similarity under the field's
metric. Until you build an approximate index, every search scans the
full-precision vector column, so a collection is searchable as soon as its
vectors are indexed and the results are exact. [Approximate search](#approximate-search)
is a commit-time option for large collections; the query does not change.

The `books` collection behind every response on this page is defined in
[Define a vector field](#define-a-vector-field) and loaded in
[Index vectors](#index-vectors) below.

## Define a vector field

The `_v` suffix stores a vector, but a searchable field needs a similarity
metric, so define it:

```http
POST /collections/books/_schema

{
  "fields": {
    "embedding_v": {
      "type": "vector",
      "dims": 3,
      "metric": "cosine"
    },
    "passages_vs": {
      "type": "vector",
      "dims": 3,
      "metric": "cosine",
      "multi": true
    }
  }
}
```

Metrics are `l2`, `ip` (inner product), and `cosine`. Set a positive `dims`
to fix the dimension for indexed and query vectors across the collection.
When omitted, the first vector in each segment establishes the dimension.

A multi-valued vector field (`multi: true`, or the `_vs` suffix) holds many
vectors per document, such as one per passage or chunk. Search collapses them
to one hit per document, scored by that document's best vector.

## Index vectors

A vector is a bare number array. A multi-valued field takes an array of
arrays, and also accepts a bare array as a one-vector list:

```http
POST /collections/books/_update

{
  "docs": [
    {
      "id": "b1",
      "title_t": "Dune",
      "author_s": "Herbert",
      "kind_s": "fiction",
      "embedding_v": [0.8, 0.1, 0.1],
      "passages_vs": [
        [0.7, 0.2, 0.1],
        [0.1, 0.8, 0.1]
      ]
    },
    {
      "id": "b2",
      "title_t": "Dune Messiah",
      "author_s": "Herbert",
      "kind_s": "fiction",
      "embedding_v": [0.6, 0.3, 0.1]
    },
    {
      "id": "b3",
      "title_t": "The Making of the Atomic Bomb",
      "author_s": "Rhodes",
      "kind_s": "nonfiction",
      "embedding_v": [0.1, 0.2, 0.7]
    }
  ],
  "commit": {}
}
```

Integer elements are accepted; doubles must narrow to finite float32.

Over gRPC, use the typed `Val.vec` and `Val.arr_vec` arms. In protobuf text,
a single-valued vector field is:

```proto
fields {
  key: "embedding_v"
  value { vec { f32 { v: 0.8 v: 0.1 v: 0.1 } } }
}
```

Populate `Val.arr_vec` with several `Vector.f32` values for a multi-valued
field, and send the request through `Indexer.Update` or `Indexer.UpdateStream`.

## Filter inside kNN

Put ordinary filters beside the query:

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
  "filter":["kind_s:fiction"],
  "limit":20,
  "fields":["id","title_t"]
}
```

```json
{
  "docs": [
    {
      "id": "b1",
      "title_t": "Dune"
    },
    {
      "id": "b2",
      "title_t": "Dune Messiah"
    }
  ]
}
```

The filter is applied during the candidate search, so `k: 20`
means twenty filtered neighbors, not twenty minus whatever a
later filter would have discarded. With an approximate index, the search
deepens its breadth automatically when a filter or multi-value collapse leaves
too few documents.

The same composition works under a boolean clause, so a vector query can be
one required clause beside text and range clauses, or the query behind a
[query facet](faceting.md#query-facets) bucket.

## Hybrid search with RRF

A `fusion` operation ranks several sources independently and merges them with
reciprocal rank fusion: each document scores `sum(1 / (k + rank))` over the
sources that returned it, ranks starting at one. Rank fusion combines BM25 and
vector similarity without converting them to a common score scale,
and facets can be computed over the fused list:

```http
POST /collections/books/_search

{
  "ops": {
    "hybrid": {
      "fusion": {
        "sources": {
          "lexical": {
            "query": {
              "match": {
                "title_t": "dune"
              }
            },
            "limit": 100
          },
          "semantic": {
            "query": {
              "knn": {
                "field": "embedding_v",
                "query": [0.75, 0.15, 0.10],
                "k": 100
              }
            },
            "limit": 100
          }
        },
        "filter": ["kind_s:fiction"],
        "rrf": {
          "k": 60
        },
        "limit": 10,
        "get_scores": true,
        "fields": ["id", "title_t"],
        "ops": {
          "authors": {
            "field_facet": {
              "field": "author_s"
            }
          }
        }
      }
    }
  }
}
```

```json
{
  "ops": {
    "hybrid": {
      "docs": [
        {
          "id": "b1",
          "title_t": "Dune",
          "_score_": 0.032786883
        },
        {
          "id": "b2",
          "title_t": "Dune Messiah",
          "_score_": 0.032258064
        }
      ],
      "ops": {
        "authors": {
          "buckets": [
            {
              "val": "Herbert",
              "count": 2
            }
          ]
        }
      }
    }
  }
}
```

Both documents came first and second in both sources, so their fused scores
are `1/61 + 1/61` and `1/62 + 1/62`. The fusion result stays under its name,
`ops.hybrid`, with the authors facet at `ops.hybrid.ops.authors`. `fusion`
has no root shorthand, so both request and response use named `ops`.

| Field | Meaning |
|---|---|
| `sources` | Named ranked inputs. Each takes its own `query`, `filter`, `sort`, and `limit`. |
| `filter` | Shared by every source, computed once per segment; ANDed with each source's own filter. |
| `rrf.k` | The rank-smoothing constant; `60` is the conventional choice. |
| `limit`, `offset`, `fields`, `get_scores`, `get_number`, `document_format` | Response shape, on the fusion, not on a source. |
| `ops` | Facets and metrics over the fused candidate set. |

A source's `limit` bounds its candidate pool: a document outside a source's
`limit` cannot contribute from that source, so size the pools for the recall
the final fused `limit` needs. `ops` under the fusion see every document in
any source's ranked list after filters, exactly the set `found` counts; the
fusion's `limit` and `offset` do not change facet counts. Fusion `offset` pages
within the union of the source windows and does not enlarge them, so deep
fusion paging needs larger source limits; per-source `offset` is ignored.
Place `ops` on the fusion itself. For facets over a full text match set, put a
sibling `top_docs` operation at the request root.

## Approximate search

Exact search costs one pass over the column per query. For large collections,
a commit can build approximate nearest-neighbor indexes (IVF+PQ, via FAISS)
as segment overlays. An overlay is built from the vector column, follows its
segment through later commits and merges, and its candidates are always
rescored from the full-precision column before they are returned. Building
an overlay does not reindex documents.

### Build at commit

Build every eligible missing vector overlay while committing:

```json
{"commit":{"build_aux_indexes":["*"]}}
```

Or select one field by its `vec.` name:

```json
{"commit":{"build_aux_indexes":["vec.embedding_v"]}}
```

Plain commits do not build overlays. A failed build leaves the commit
unpublished and can be retried. Once a segment has an overlay, later commits
carry it forward and delete-only commits keep it (deleted documents are
filtered out at query time).

Whether a segment gets an overlay is size based. IVF+PQ is built only when the
segment has enough vectors to train on and enough scan cost to justify it;
smaller segments stay on the exact column scan. A single query routinely uses
the index for large segments and exact scan for small ones, with the same
results contract either way. A merged segment gets fresh
overlays for the fields being built, when it passes the same
thresholds. If a merge-time build fails, the merged segment is published
without the overlay and that field is served by exact scan until a later
explicit build succeeds.

### Query knobs

Knobs whose units belong to one index type live in a sub-object named after
it; `ivf` is the only one today. Knobs at the top level of `knn` apply to
every index type.

```json
{
  "query": {
    "knn": {
      "field":"embedding_v",
      "query":[0.75,0.15,0.10],
      "k":20,
      "refine_candidates":400,
      "ivf": {"nprobe":32}
    }
  }
}
```

| Field | Meaning |
|---|---|
| `field` | Vector field. Required. |
| `query` | Query vector; must match the field's dimension. Required. |
| `k` | Nearest documents the query node produces. Required, positive. |
| `exact` | Require the true top-k. Uses the full-precision column scan and ignores overlays and the knobs below. |
| `refine_candidates` | Approximate candidates collected before the full-precision rescore. Unset: sized from `k` (see below). Explicit: exactly that many, clamped up to `k`. |
| `ivf.nprobe` | IVF effort, merge-stable (see below). `0` or absent: adaptive, may deepen automatically. |
| `ivf.min_scan_fraction` | A floor on the fraction of live vector values scanned, in `[0,1]`; `0.001` means 0.1%. |

`refine_candidates` sizes the host-side candidate pool, so it means the same
thing whichever index produced the candidates. When unset, the pool is affine
in `k`, with a multiplier that decays from about 10 at `k=1` to a small floor
by `k=1000`: quantization mis-ranks a true neighbor by a roughly constant
number of positions regardless of `k`, so small `k` needs fixed headroom, while
large-`k` requests are recall-oriented retrieval whose near-tied tail does not
benefit from extra overfetch. The final pool is always rescored from the
full-precision column, sorted by exact score, and collapsed to one hit per
document.

`ivf.nprobe` is interpreted as the number of lists
Luxir would probe if the field were a single IVF index built with
`nlist = sqrt(live_vector_count)`, capped the same way as the builder's
`nlist`. That total effort is spread across the current per-segment indexes,
so a merge does not change the effort a request asks for. An explicit `nprobe`
pins the effort cap; an adaptive one may deepen when filters or collapse
underfill the result. Probed lists are always scanned in full; `nprobe` is the
only work limiter.

### Scoring

L2 scores are `1 / (1 + squared_distance)`. Inner product scores are the dot
product. Cosine is inner product over normalized vectors. The score
distribution depends on the embedding model and corpus, so calibrate
thresholds per deployment.

Cosine fields normalize on write by default (`normalize_on_write: true`), so
reading the vector column back returns the unit vector rather than the bytes
you submitted. Vector fields are returned only when named in `fields`:

```http
POST /collections/books/_search

{"query": "id:b2", "fields": ["id", "embedding_v"]}
```

```json
{
  "docs": [
    {
      "id": "b2",
      "embedding_v": [0.8846517, 0.44232586, 0.14744195]
    }
  ]
}
```

That is `[0.6, 0.3, 0.1]` at unit length. Set `normalize_on_write: false` to
keep the raw vectors in the column; candidate rescoring then normalizes at
query time. Zero and near-zero cosine vectors have no direction and are
skipped at index time.

### Measure recall

IVF+PQ is approximate. Higher `ivf.nprobe` and higher `refine_candidates`
raise recall and latency. Setting `nprobe` to at least the reference
`sqrt(live_vector_count)`, or `ivf.min_scan_fraction` to `1`, makes the search
exhaustive over IVF lists, but PQ ordering still decides which candidates are
rescored.

`exact: true` uses a path that guarantees the true top-k and does not
consult overlays. Everything else about
the query is identical (filters, multi-valued collapse, score scale), which
makes it the built-in ground truth for measuring your ANN settings. Run the
same query twice, once with `exact: true`, and compare the two lists.

Results are deterministic. A query run with intra-request parallelism returns
bit-identical results to a serial run of the same query.

## Limits

- IVF+PQ is the only approximate index type. There is no HNSW yet.
- Filters are always applied inside the vector search. There is no cost-based
  choice between filtered approximate search and exact search over the
  filtered set, and no automatic exact fallback when a selective filter still
  starves the approximate search after breadth deepening. Use `exact: true`
  when exact filtered top-k is the required contract.
- There is no public command to remove or rebuild an overlay that a segment
  already has. New and merged segments build from their columns, and a segment
  without an overlay can receive one through a later selected commit.
- Skipped zero-norm cosine vectors are reported only as a server-log warning,
  not in the update response.
- Vector fields are returned only when named in `fields`; they are excluded
  from default projection.
