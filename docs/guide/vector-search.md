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

A list's [diversify cap](searching.md#diversifying-a-document-list) applies
after the kNN window is selected. It does not enlarge `k` or refill the window:
several neighbors with the same key can therefore leave fewer returned rows.
A fusion source can use the same cap before its documents enter fusion.

The `books` collection behind every response on this page is defined in
[Define a vector field](#define-a-vector-field) and loaded in
[Index vectors](#index-vectors) below.

## Nearest groups

Use `knn.unit: "groups"` with [collapse](searching.md#collapsing-results)
to retrieve the nearest products, documents or other entities when each has
several vector-bearing rows:

```json
{
  "query": {
    "knn": {
      "field": "embedding_v",
      "query": [0.75, 0.15, 0.10],
      "k": 20,
      "unit": "groups"
    }
  },
  "collapse": "product_id_s",
  "limit": 10,
  "get_scores": true
}
```

Here `k` selects up to 20 groups and the list returns the first 10. Repeated
near neighbors from one product do not consume other products' places. A
group's similarity is its best eligible member's similarity; equal scores
follow that member's document order. Filters and deletes apply before group
selection. The collapse key can be a tuple, and its `missing` policy also
applies inside vector search.

The query's match set contains one scored witness per selected group. Facets,
metrics and child operations see those witnesses, rather than every member
of their groups. Multi-valued vectors first use the document's maximum
similarity, then documents compete within their groups.

The unit belongs to the kNN node. Boolean and rescore wrappers preserve it.
`offset` and `limit` never enlarge `k`; a page beyond that candidate window
is empty. With the default `unit: "docs"`, `k` still selects documents before
list collapse, so repeated keys can leave fewer than `k` groups. `groups`
without an enclosing collapse is a request error.

`exact: true` finds the exact nearest groups and their best members. ANN
uses group-aware admission while scanning and refining candidates, but
both group recall and the choice of representative can be approximate.
`refine_candidates` counts candidate documents in both units. Group search
limits how many candidates each group can contribute, keeping several members
when the budget permits so full-precision rescoring can recover the best
representative. A larger pool can improve both group and representative recall
at a cost in work and memory. Exact segment results bypass the approximate
candidate budget and compete with approximate candidates after rescoring.

Search deepens under the existing candidate and IVF effort caps. When it
returns fewer than `k` groups, the final response includes a warning with
code `knn_group_underfill`. Its message reports the requested and returned
counts and `candidates_exhausted`: `true` means no more eligible candidates
remain; `false` means a cap or search-effort limit stopped the search.
Finding `k` groups does not certify ANN recall.

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

An optional `ann` object chooses how [approximate indexes](#index-settings)
for the field are built; the defaults suit most embedding models.

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

Integer elements are accepted. Every element must be a finite float32: a double
past float32 range, or a NaN or infinity in any arm (typed gRPC vectors included),
fails the document.

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

## Hybrid search

A `fusion` operation ranks several sources independently and merges them into
one list. It takes exactly one fusion method: `rrf` (reciprocal rank fusion,
below) or `linear` (see [Linear fusion](#linear-fusion)). With reciprocal rank
fusion each document scores `sum(weight / (k + rank))` over the sources that
returned it, ranks starting at one. Rank fusion combines BM25 and
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
| `weights` | Per-source weights keyed by source name; a source without an entry has weight `1`. |
| `rrf.k` | The rank-smoothing constant, `>= 0`. Omitted selects `60`, the conventional choice. |
| `linear.normalization` | Score normalization for [linear fusion](#linear-fusion); `min_max`, the default, is the only one today. |
| `limit`, `offset`, `fields`, `get_scores`, `get_number`, `document_format` | Response shape, on the fusion, not on a source. |
| `ops` | Operations over the fused candidate set, including collapsed `top_docs`. |

A source's `limit` bounds its candidate pool: a document outside a source's
`limit` cannot contribute from that source, so size the pools for the recall
the final fused `limit` needs. `ops` under the fusion see every document in
any source's ranked list after filters, the fused candidate set; the
fusion's `limit` and `offset` do not change facet counts. Fusion `offset` pages
within the union of the source windows and does not enlarge them, so deep
fusion paging needs larger source limits; per-source `offset` is ignored.
Place `ops` on the fusion itself. For facets over a full text match set, put a
sibling `top_docs` operation at the request root.

A collapsed `top_docs` in `Fusion.ops` is a separate list over that entire
candidate set, before Fusion paging. It ranks by its own query or sort and
collapses by its own key; it does not change the fused ranking. For example:

```json
{
  "ops": {
    "hybrid": {
      "fusion": {
        "rrf": {},
        "sources": {"text": {"query": "title_t:jacket", "limit": 100}},
        "limit": 10,
        "ops": {
          "products": {
            "top_docs": {"collapse": "product_s", "sort": "price_i asc", "limit": 10}
          }
        }
      }
    }
  }
}
```

A source can use `collapse` for diversity: its `limit` then counts groups,
and its best member documents enter ordinary document fusion; under
`linear`, the source normalizes its head scores over its group list. Sources
may use independent keys and missing policies. Source collapse accepts only
`field` and `missing`; `head_sort`, metric `sort`, `get_number` and `ops` are
errors. A source's `knn.unit: "groups"` uses that source's collapse key.

`fusion.collapse` groups the fused document list before paging. Groups rank
by their best member's fused score, then that document's order. `offset`,
`limit` and `found` count groups; `found_docs` counts eligible fused
documents. `missing: "drop"` removes keyless candidates after fusion without
refilling source windows. Each head's `_score_` is its own fused document
score; `_group_.rank` contains the best member's fused score when scores are
requested.
As under a collapsed `top_docs`, facets in the fusion's `ops` then count
groups by default (`count_unit: "docs"` counts documents), and a source's
`knn.unit: "groups"` without its own collapse uses the fusion's key.

```json
{
  "ops": {
    "hybrid": {
      "fusion": {
        "rrf": {},
        "sources": {
          "text": {"query": "title_t:jacket", "collapse": "product_s", "limit": 100},
          "vector": {
            "query": {"knn": {"field": "embedding_v", "query": [0.2, 0.8], "k": 100}},
            "limit": 100
          }
        },
        "collapse": {
          "field": "product_s", "head_sort": "price_i asc", "get_number": true,
          "ops": {"colors": {"field_facet": {"field": "color_s"}}}
        },
        "limit": 10, "get_scores": true, "get_number": true
      }
    }
  }
}
```

Fused-group counts, display heads and per-group operations use only documents
in the union of the source windows, with each document counted once. A `domain`
reset beneath a group can recover all members, just as under a collapsed
`top_docs`. Fusion's ordinary `ops` keep the full evidence union, including
candidates excluded by the outer collapse's missing policy. Metric group order
(`fusion.collapse.sort`) is currently an error.

A Fusion operation can itself appear in a collapsed list's ordinary `ops`; its source
domain is that list's matching documents, and Fusion still ranks documents.

### Source weights

`weights` scales each source's contribution to `weight / (k + rank)`, so a
source can count for more or less than the others without changing the
candidate pools:

```json
{
  "fusion": {
    "sources": {
      "lexical": {"query": {"match": {"title_t": "dune"}}, "limit": 100},
      "semantic": {"query": {"knn": {"field": "embedding_v", "query": [0.75, 0.15, 0.10], "k": 100}}, "limit": 100}
    },
    "weights": {"lexical": 1, "semantic": 2},
    "rrf": {}
  }
}
```

A document ranked first by both sources here scores `1/61 + 2/61`. Weights
must be finite, `>= 0`, and within float range, every key must name a source,
and at least one source needs a weight above zero; anything else rejects the
request. A weight of `0` disables its source: the source is not run, and its
documents do not reach the fused list, `found`, or the fusion's `ops`. That
lets a client turn a source off by configuration without rewriting the
request.

### Linear fusion

`linear` combines the sources' scores instead of their ranks. Each source's
scores are rescaled to `[0, 1]` over its ranked list, then summed with the
source weights:

```text
normalized_s(d) = (score_s(d) - min_s) / (max_s - min_s)
fused(d)        = sum over sources s of weight_s * normalized_s(d)
```

`min_s` and `max_s` are the lowest and highest scores in source `s`'s ranked
list, that is its top `limit` documents after filters, merged across every
segment. A source's best document gets `1` and its worst gets `0`, whatever
the score scale: BM25, a dot product that can be negative, or a rescore
expression. With two sources, weights `alpha` and `1 - alpha` give the
familiar convex combination.

```json
{
  "fusion": {
    "sources": {
      "lexical": {"query": {"match": {"title_t": "dune"}}, "limit": 100},
      "semantic": {"query": {"knn": {"field": "embedding_v", "query": [0.75, 0.15, 0.10], "k": 100}}, "limit": 100}
    },
    "weights": {"lexical": 0.3, "semantic": 0.7},
    "linear": {"normalization": "min_max"}
  }
}
```

`"linear": {}` selects the same `min_max` normalization. Edge cases:

- A source that did not return a document contributes `0` to it.
- A source with no documents contributes nothing.
- A list whose scores are all equal, including a single document, normalizes
  every document to `1`: each is the source's top match. A filter-like source
  (say a `constant_score` query) therefore adds its full weight to every
  document it returns.
- Non-finite scores are mapped before `min_s` and `max_s` are taken, and
  those cover only finite scores: `NaN` contributes `0` (the document stays a
  candidate), `+inf` normalizes to `1`, and `-inf` to `0`.

The candidate set is the same as with `rrf`: `found` and the fusion's `ops`
see every document in any enabled source's list, including documents whose
fused score is `0`. Ties in the fused score fall back to index order.

Every source must rank by relevance score: a `sort` other than `score`
descending (optionally followed by `_docid_` ascending) rejects the request,
since a field-sorted list has no score to normalize. Use `rrf` to fuse sorted
sources.

Prefer `rrf` when the score distributions are unknown or unstable, when a
source is field-sorted, or as a robust default that needs no tuning. Prefer
`linear` when score magnitudes carry information that ranks discard (a clear
winner in one source should beat a near tie in another) and you can tune the
weights against relevance judgments. Min-max normalization is sensitive to
outliers and to the pool size: one extreme score compresses the rest of its
list, and a larger source `limit` usually lowers `min_s`, so retune weights
after changing source limits.

## Approximate search

Exact search costs one pass over the column per query. For large collections,
a commit can build approximate nearest-neighbor indexes as segment overlays:
an inverted-file (IVF) index whose lists hold product-quantization (PQ) codes
by default, or opt-in RaBitQ codes, built with FAISS. An overlay is built
from the vector column, follows its segment through later commits and
merges, and its candidates are always rescored from the full-precision column
before they are returned. Building an overlay does not reindex documents.

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

Whether a segment gets an overlay is size based. An overlay is built only
when the segment has enough vectors (8192 for RaBitQ, about 10,000 for PQ,
which must train its codebooks) and enough scan cost to justify it; smaller
segments stay on the exact column scan, which is faster than an index at that
size and exact. A single query routinely uses
the index for large segments and exact scan for small ones, with the same
results contract either way. A merged segment gets fresh
overlays for the fields being built, when it passes the same
thresholds. If a merge-time build fails, the merged segment is published
without the overlay and that field is served by exact scan until a later
explicit build succeeds.

### Index settings

A vector field's `ann` object selects the overlay codec and its parameters:

```http
POST /collections/books/_schema

{
  "fields": {
    "embedding_v": {
      "type": "vector",
      "dims": 768,
      "metric": "cosine",
      "ann": {"codec": "pq"}
    }
  }
}
```

| Field | Meaning |
|---|---|
| `codec` | `pq` (default) or opt-in `rabitq`. |
| `nlist` | IVF lists per segment overlay. Absent or `0`: `sqrt(segment vectors)`, capped at 4096. |
| `rabitq.bits` | Bits per dimension, `1` to `9`. Default `1`. |
| `rabitq.query_bits` | Bits the query is quantized to while scanning, `0` to `8`; `0` scans with the float query. Default `4`. |
| `pq.m` | PQ sub-quantizers. Positive values require positive effective (inherited) `dims` and must divide it. Absent or `0`: automatic, including inferred dims; the largest divisor leaving at least 4 dimensions each, capped at `max(32, dims / 8)` for IP/cosine or `32` for L2. If none does, use the largest divisor under the same cap. |
| `pq.bits` | Bits per PQ code, `1` to `8`. Default `8`. |

`rabitq` settings require an explicit `"codec":"rabitq"`, for example
`"ann":{"codec":"rabitq","rabitq":{"bits":1,"query_bits":4}}`.
Omitting the codec selects PQ, including for `ann:{}`, `nlist` alone, or
`pq` settings alone; a `rabitq` object does not infer the codec. A field
without `ann` inherits its parent's, and an `ann` object replaces the
parent's whole definition rather than merging with it.

These rules also apply to inherited ANN settings and vector variants. Use
automatic `m` when dimensions are inferred from indexed values; an explicit
positive `m` requires dimensions pinned in the schema.

**RaBitQ** (opt-in) stores each vector as one sign bit per dimension of
its randomly rotated offset from its IVF list's centroid, plus a few
correction factors: 104 bytes for a 768-dimension vector, about 1/30 of the
float vector. Distances estimated from these codes come with a proven error
bound, and the codec trains nothing beyond the IVF centroids. With `bits`
above 1 (extended RaBitQ), each dimension gets extra magnitude bits; a scan
then estimates every vector from its sign bits first and spends the extra bits
only on vectors whose error bound still overlaps the current top candidates.
`query_bits: 4` matches the 1-bit-document, 4-bit-query asymmetry other
engines use and costs no measurable recall against `8`.

**PQ** (the default) splits each vector into `m` sub-vectors and stores one learned
codebook entry per sub-vector. PQ's automatic `m` scales with IP/cosine
dimensions above 256: for example, 768 dimensions selects `m=96`, or 96 code
bytes at 8 bits, close to RaBitQ's 104. This improves high-dimensional recall
over `m=32` at the cost of larger codes and more scan work; see the
[codec measurements](../design/vector-search.md#codec-measurements).
L2 keeps the cap of 32 because FAISS retains an `nlist * m * 256`-float
precomputed table at 8 bits; IP/cosine builds its lookup table per query and
has no such resident table. Fields of up to 256 dimensions and explicit `m`
settings keep their existing choices. PQ codebooks are retrained for every
segment build and merge; automatic `m` depends only on dimensions and metric.

PQ favors scan speed: FAISS currently scans PQ lists faster than RaBitQ
lists. Opt into RaBitQ for its different size/recall tradeoff.
With 1-bit RaBitQ, recall is usually limited by how many lists are probed
rather than by the codes, so raise `ivf.nprobe` first; extra `bits` help mainly
when `refine_candidates` is small.

Overlays record the settings they were built with, so a change applies to
segments built afterwards: new flushes, merges, and segments without an
overlay that a later commit selects. A query serves every segment with its own
overlay's codec, so segments built under different settings mix freely.
`query_bits` is applied at search time to both document and group searches
and takes effect for existing RaBitQ overlays immediately, including when the
current schema selects PQ for future builds. Changing `ann` does not change
the field's identity: existing values stay searchable.

The [resolved schema view](schema.md#resolved-view) exposes each vector
representation's effective `ann` settings, including defaults and inherited
values. It includes only the selected codec's settings. `nlist:0` and `m:0`
remain automatic policies, rather than sizes chosen for any one segment.

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
| `k` | Nearest documents or groups the query node produces. Required, positive. |
| `unit` | `docs` (default) or `groups`; groups requires an enclosing collapse. |
| `exact` | Require the true top-k. Uses the full-precision column scan and ignores overlays and the knobs below. |
| `similarity` | Minimum similarity of a hit, in the metric's own space; see [Similarity threshold](#similarity-threshold). |
| `refine_candidates` | Candidate documents collected before the full-precision rescore, in both units. Unset: sized from `k` (see below). Explicit: exactly that many, clamped up to `k`. |
| `ivf.nprobe` | IVF effort, merge-stable (see below). `0` or absent: adaptive, may deepen automatically. |
| `ivf.min_scan_fraction` | A floor on the fraction of live vector values scanned, in `[0,1]`; `0.001` means 0.1%. |

`refine_candidates` sizes the candidate-document pool in both units, whichever
index produced the candidates. When unset, the pool is affine
in `k`, with a multiplier that decays from about 10 at `k=1` to a small floor
by `k=1000`: quantization mis-ranks a true neighbor by a roughly constant
number of positions regardless of `k`, so small `k` needs fixed headroom, while
large-`k` requests are recall-oriented retrieval whose near-tied tail does not
benefit from extra overfetch. The final pool is always rescored from the
full-precision column, sorted by exact score, and collapsed to one hit per
document (or per group with `unit: "groups"`).

`ivf.nprobe` is interpreted as the number of lists
Luxir would probe if the field were a single IVF index built with
`nlist = sqrt(live_vector_count)`, capped the same way as the builder's
`nlist`. That total effort is spread across the current per-segment indexes,
so a merge does not change the effort a request asks for. An explicit `nprobe`
pins the effort cap; an adaptive one may deepen when filters or collapse
underfill the result. The adaptive default starts at `sqrt(nlist)` of that
reference index for fields of up to 128 dimensions and scales by
`sqrt(dims / 128)` above that (capped at 4x). This effort policy applies to
both codecs. The multiplier is about 2.5x at 768 to 960 dimensions; RaBitQ
measurements on GIST and Cohere embeddings showed recall@10 rising from
0.82-0.87 to about 0.95. Probed lists are always scanned in full; `nprobe` is
the only work limiter.

### Scoring

L2 scores are `1 / (1 + squared_distance)`. Inner product scores are the dot
product. Cosine is inner product over normalized vectors. The score
distribution depends on the embedding model and corpus, so calibrate
thresholds per deployment.

### Similarity threshold

`similarity` drops the nearest neighbors that are not similar enough, with the
semantics of Elasticsearch's `knn.similarity`. The value is in the metric's
space, not the score's:

| Metric | `similarity` means | Hit kept when |
|---|---|---|
| `l2` | Maximum L2 distance (not squared); must be `>= 0` | `distance <= similarity` |
| `ip` | Minimum dot product | `dot >= similarity` |
| `cosine` | Minimum cosine similarity | `cosine >= similarity` |

```json
{"query": {"knn": {"field": "embedding_v", "query": [0.75, 0.15, 0.10], "k": 20, "similarity": 0.8}}}
```

The threshold trims the `k` nearest documents after the full-precision
rescore; it does not search past them, so a request can match fewer than `k`
documents. The comparison uses each document's best vector, before any
enclosing `boost`. Trimmed documents are not matches: they leave `found`,
facets, and metrics as well as the returned list, on exact and approximate
segments and in parallel execution alike.

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

Approximate search is approximate. Higher `ivf.nprobe` and higher
`refine_candidates` raise recall and latency. Setting `nprobe` to at least the
reference `sqrt(live_vector_count)`, or `ivf.min_scan_fraction` to `1`, makes
the search exhaustive over IVF lists, but the codec's estimated ordering still
decides which candidates are rescored.

`exact: true` uses a path that guarantees the true top-k and does not
consult overlays. Everything else about
the query is identical (filters, multi-valued collapse, score scale), which
makes it the built-in ground truth for measuring your ANN settings. Run the
same query twice, once with `exact: true`, and compare the two lists.

Results are deterministic. A query run with intra-request parallelism returns
bit-identical results to a serial run of the same query.

## Limits

- IVF (with RaBitQ or PQ codes) is the only approximate index type. There is
  no HNSW yet.
- An `ann` change does not rebuild existing overlays; only segments built
  afterwards use it (see [Index settings](#index-settings)).
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
