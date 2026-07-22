# Searching

A Solux search request describes the result, not a sequence of calls. The
small form asks for one ranked document list. The full form names several
operations and nests facets or metrics under the query whose match set they
should consume. Both are the same model: the small form becomes a `top_docs`
operation named `q`.

All HTTP searches use:

```
POST /collections/{collection}/_query
```

## One result list

The common request is deliberately shallow:

```http
POST /collections/books/_query
Content-Type: application/json

{
  "query": "title_w:(dune OR messiah) AND year_i:>=1965",
  "filter": [
    {"name": "available", "query": "stock_i:>0"}
  ],
  "limit": 10,
  "get_number": true,
  "get_scores": true,
  "fields": ["id", "title_w", "year_i", "price_f"]
}
```

```json
{
  "found": 2,
  "docs": [
    {"id":"b1","title_w":"dune","year_i":1965,"price_f":9.99,"_score_":0.73},
    {"id":"b2","title_w":"dune messiah","year_i":1969,"price_f":8.99,"_score_":0.61}
  ]
}
```

`filter` clauses constrain matches but do not contribute to the score. They
are named so the same logical constraint has an identity wherever domains are
composed. An expression string is usually the clearest filter syntax.

The top-document fields are:

| Field | Meaning |
|---|---|
| `query` | One structured query object or an expression string. Required. |
| `filter` | Named, non-scoring queries ANDed with the main query. |
| `limit` | Maximum documents returned. Default `10`; `0` for count/analytics only; `-1` for all matches. |
| `get_number` | Compute and return the exact match count as `found`. |
| `get_scores` | Add `_score_` to every returned document. |
| `fields` | Fields to retrieve. |
| `sorts` | Value expressions used as sort keys. No list means relevance order. |
| `batch_size` | Maximum documents in one streaming response batch. |
| `document_format` | `rows` or `columns`; HTTP defaults to rows, gRPC to columns. |
| `ops` | Facets or metrics over this query's complete match domain. |

`offset` exists on the wire but is not applied by the current collector. Do
not use it for pagination. Ordered page-after pagination is not implemented;
use document-per-line export when the goal is to consume every match.

## Query forms

Every query is one node and every node can appear under boolean clauses,
filters, fusion sources, or another query wrapper.

| Query | Example | Notes |
|---|---|---|
| Match all | `{"all":true}` | Constant-scoring all-documents query. |
| Match | `{"match":{"title_w":"darkness"}}` | Analyzes text; the object shown is sugar for explicit `field` and `val`. |
| Exists | `{"exists":{"field":"year_i"}}` | Supplied value exists; expression shorthand is `year_i:*`. |
| Phrase | `{"phrase":{"field":"title_w","text":"left hand","slop":0}}` | Position-aware; see the [query language](query-language.md#terms-and-phrases) for slop semantics. |
| Range | `{"range":{"field":"year_i","gte":1960,"lt":1970}}` | Numeric, date, string, ID, or text term ranges. |
| Prefix | `{"prefix":{"field":"title_w","prefix":"dark"}}` | Term prefix, not a general wildcard. |
| Fuzzy | `{"fuzzy":{"field":"author_s","term":"leguin","max_edits":1}}` | Closest-term rewrite; `max_expansions` defaults to `50`. |
| Boolean | `{"boolean":{"required":[...],"filter":[...],"optional":[...],"prohibited":[...],"min_match":1}}` | Uniform composition of scoring and non-scoring clauses. |
| Constant score | `{"constant_score":{"query":...,"score":1.0}}` | Replaces child scores without changing its match set. |
| Boost | `{"boost":{"query":...,"boost":2.0}}` | Multiplies child scores. A numeric `boost` sibling is input sugar. |
| Simple query | `{"simple_query":{"q":"dune | messiah","fields":["title_w"]}}` | Never-failing syntax for raw search-box input. |
| Expression | `"title_w:dune AND year_i:>=1965"` | Strict developer syntax with exact parse errors and safe variables. |
| kNN | `{"knn":{"field":"embedding_v","query":[...],"k":20}}` | Exact or ANN vector search; see [Vector Search](vector-search.md). |
| Geo box | `{"geo_box":{"field":"location","min_lat":40,"max_lat":42,"min_lon":-75,"max_lon":-72}}` | Inclusive box, including dateline-crossing boxes. |
| Geo distance | `{"geo_distance":{"field":"location","lat":40.71,"lon":-74.01,"radius_meters":5000}}` | Inclusive great-circle radius. |

`match` accepts `operator: "and"` or a numeric `min_match` when analysis
produces several terms. Boolean `optional` clauses rank but do not constrain a
query that already has `required` or `filter` clauses unless `min_match` is set.

The [structured query reference](query-reference.md) documents every field and
default. Use [`simple_query`](query-reference.md#simple-query) for text a person
typed and the strict [Solux query language](query-language.md) for text your
application authored. That separation is intentional: one degrades rather than
fail; the other would rather report the exact byte offset of a bug than guess.

## Counts and top-k work

`found` is opt-in. Without `get_number`, Solux can use block score bounds and
MaxScore-style skipping to avoid scoring documents that cannot enter the top
k. With `get_number`, it exhaustively counts the domain and returns the exact
total. The request says which contract it consumes; there is no hidden
estimated-count substitution.

For a pure count plus facets or metrics, set `limit: 0` and `get_number: true`.
No document fields are loaded.

## Field retrieval and result shape

`fields` projects stored or column-backed values into each hit. HTTP defaults
to row documents: a missing field is an absent key. Set
`document_format: "columns"` when consumers prefer every supported projected
key in every row, with missing cells rendered as `null` by HTTP.

The underlying gRPC response remains a typed `DocList`: dense columns and row
maps can coexist, `row_count` is authoritative, and `_score_` is a synthetic
float column when requested.

## Sorting

Sort a column field explicitly:

```json
{
  "query": {"all":true},
  "sorts": [{"expr":"year_i","dir":"desc"}],
  "fields": ["id","title_w","year_i"]
}
```

The `expr` member accepts either a bare field name or a numeric value expression.
Numeric, date, string, and ID field names retain the direct column-sort path.
Use `col("name")` when a field name is reserved or is not an identifier. Analyzed
text has no sortable value unless it is indexed for string sorting or copied to
a `string` column. Documents missing the sort value always sort last, under
both directions. Array values produced inside a composed expression require
an explicit reducer.

A bare multi-valued string field sorts by its smallest value ascending and its
largest value descending (string values are stored as a per-document sorted
set, so either edge is free to read). A bare multi-valued numeric field sorts
by its FIRST stored value: numeric values are stored in insertion order, so
the first value is the one key that needs no per-document scan. Sort by
`min(f)` or `max(f)` when you want edge semantics on a numeric field and are
willing to pay the scan.

Value expressions support numeric constants, `$name` values from the sort's
`vars` map, the reserved `score` leaf, and these functions:

- Arithmetic: `add`, `sub`, `mul`, and `div`.
- Defaults: `def(value, fallback)` substitutes only when `value` is missing.
- Unary math: `neg`, `abs`, `sqrt`, `log`, and `log1p`.
- Multi-valued reducers: `min`, `max`, and `avg`. A composed array-valued root
  must use one of these explicit reducers. The two-argument `min` and `max`
  forms compare scalar values and are useful for clamping.

For example:

```json
{
  "expr": "add(popularity_i,mul(score,$weight))",
  "vars": {"weight": 0.25},
  "dir": "desc"
}
```

Integer-only arithmetic remains int64; a double operand promotes that operation
to double. Array arithmetic permits scalar broadcasting but does not implicitly
zip two arrays. NaN, infinity, invalid math domains, division by zero, and int64
overflow are rejected rather than becoming sortable sentinels.

Missing values sort last in both directions. Several sort specifications form
an ordered lexicographic sort. `score` and `_score_` sort by the query score, and
`_docid_` sorts by reader-local `(segment, docid)` order; any can appear in any
position.

An omitted direction defaults to descending for `score`/`_score_` and ascending
for every other key. After all explicit components tie, results use
`(segment, docid)` ascending as the final deterministic tiebreak. This reader-
local identity can change after segment merges, so it is not a durable
pagination token.

## One request, several results

Use the full request when the response should include analytics as well as
documents. The simple fields move unchanged one level under `top_docs`:

```http
POST /collections/books/_query

{
  "ops": {
    "results": {
      "top_docs": {
        "query": "title_w:dune",
        "filter": [{"name":"available","query":"stock_i:>0"}],
        "limit": 10,
        "get_number": true,
        "fields": ["id","title_w","price_f"],
        "ops": {
          "categories": {
            "field_facet": {"field":"category_s","limit":10}
          },
          "average_price": {
            "gen_op": {"name":"avg","args":["price_f"]}
          }
        }
      }
    }
  }
}
```

```json
{
  "found": 42,
  "docs": [{"id":"b1","title_w":"dune","price_f":9.99}],
  "ops": {
    "categories": {
      "buckets": [
        {"val":"science-fiction","count":31},
        {"val":"classic","count":18}
      ]
    },
    "average_price": 11.72
  }
}
```

The document list is promoted to `found` and `docs` in the HTTP envelope;
nested and sibling operation results appear under `ops`. `avg`, `min`, and
`max` generic operations work on numeric columns and ignore missing values.
An empty metric domain renders as `null` in JSON.

See [Faceting](faceting.md) for terms, range, date, nested, and per-bucket
operations. See [Vector Search](vector-search.md#hybrid-search-with-rrf) for a
full fusion operation with several ranked sources.

## Request-level controls

Request-wide settings require the full form because the shorthand root is a
`top_docs` message:

```json
{
  "request_id": "search-42",
  "freshness_us": 50000,
  "time_zone": "America/Denver",
  "profile": true,
  "max_parallel": 1,
  "ops": {
    "results": {"top_docs": {"query":"published_dt:>=NOW/DAY","limit":10}}
  }
}
```

`freshness_us` bounds how stale an index view may be; `0` requires the latest
commit. `time_zone` supplies the civil frame for every date query and facet in
the request. `max_parallel: 0` lets the engine choose intra-request parallelism
and is the default; `1` executes the request on one worker thread; `-1`
executes it inline on the transport thread that received it (no scheduler
handoff at all - useful for isolating scheduling overhead, at the cost of
blocking that connection's thread). Values above `1` are reserved and
currently rejected. The path collection overrides any
collection target in the HTTP body.

`request_id` is echoed by gRPC responses for correlation within a bidirectional
stream. The custom HTTP response envelope does not currently include it.

`profile: true` returns an execution profile on the final response. Profiling
is opt-in and request-wide, but operations adopt instrumentation independently;
string field facets are the currently instrumented operation. Their profile
contains one entry per executed segment with the selected counter strategy,
selection inputs such as cardinality and domain size, thread ID, elapsed
microseconds, and human-readable details:

```json
{
  "profile": {
    "ops": [{
      "name": "categories",
      "pieces": [{
        "kind": "segment",
        "segment": 0,
        "max_doc": 100000,
        "strategy": "skinny",
        "cardinality": 12000,
        "domain_size": 84000,
        "thread_id": 123,
        "elapsed_us": 714,
        "details": ["all-docs domain, bulk column scan"]
      }]
    }]
  }
}
```

Treat `strategy` and the typed numeric fields as diagnostics, not a stable
performance promise. `details` is deliberately human-readable and must not be
machine-parsed. Use `max_parallel: 1` when comparing segment timings without
parallel scheduling as a variable.

## Streaming HTTP responses

The normal HTTP response is chunked NDJSON. Each line is a complete response
envelope for one batch, and `more: true` says that at least one more line is
coming. `batch_size` places an upper bound on documents in one engine batch;
the server may choose a smaller batch to protect memory. Producers pause when
the connection's buffered output crosses its configured high-water mark, so a
slow reader applies backpressure instead of accumulating the result in RAM.

For small default-limit requests there is normally one line, which is why it
also looks like an ordinary JSON response to simple tools.

### Stream every match

Add `?format=docs` and set `limit: -1` when the consumer wants documents rather
than response envelopes:

```bash
curl -s 'http://localhost:9400/collections/books/_query?format=docs' \
  -H 'Content-Type: application/json' \
  -d '{"query":{"all":true},"limit":-1,"fields":["id","title_w"]}'
```

Each line is one bare document. If `get_number` or warnings are present, a
leading `_header_` record carries them. Multi-operation document streams use
header markers to name the following operation.

Docs format accepts only `top_docs` and fusion operations, uses row documents,
and rejects nested operations because it has no envelope in which to put them.
It is an export/ETL format, not an alternate analytics response.

No cursor or server-side scroll state is created. A clean HTTP chunk
terminator means the result set completed; a failure after output began aborts
the chunked body so a conforming client sees truncation rather than a plausible
partial success. The output can be piped directly into the
[NDJSON update endpoint](indexing.md#unbounded-ndjson-ingest).

## Warnings and errors

Warnings declare a request that succeeded with a bounded degradation. For
example, `fuzzy_scoring_truncated` means the scoring-clause budget selected
fewer expansions than the query requested:

```json
{"warnings":[{"code":"fuzzy_scoring_truncated","message":"..."}]}
```

Treat warning `code` as the machine key and `message` as human detail. The
engine does not silently drop or clamp query behavior. A lower operator limit
uses the distinct `fuzzy_clamped` code.

Unknown JSON keys, invalid enums, wrong field types, and expression syntax
errors are rejected. Before execution, `?explain=request` returns the canonical
request that the server parsed; posting that body back executes the same
request. This expands JSON shorthand, but it is not a post-analysis query plan.
