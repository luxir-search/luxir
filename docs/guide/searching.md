# Searching

A search request describes the data you want back: the ranked documents and,
when you ask for them, the facets and metrics that go with them, all computed
in one request over one consistent view of the index.

The simple form consists of a handful of keys at the top level.
The full form allows any number of independent search operations.

```
GET  /collections/{collection}/_search
POST /collections/{collection}/_search
```

## Queries and search operations

A **query** ([`Query`](../reference/protobuf.md#message-luxir.query)) defines which
documents match and how they score.
`match`, `range`, `boolean`, and `knn` are query kinds. Queries compose with
other queries, and a filter uses a query without contributing to the score.

A **search operation** ([`SearchOp`](../reference/protobuf.md#message-luxir.searchop))
defines what to compute or return:
`top_docs` returns ranked documents for a query, facets return buckets, `expr_op` computes
metrics, and `fusion` combines ranked lists. A request
([`SearchRequest`](../reference/protobuf.md#message-luxir.searchrequest))
contains a map of named operations in `ops`.

`top_docs` ([`TopDocs`](../reference/protobuf.md#message-luxir.topdocs)) connects the
two: its `query` and `filter` select the documents, while `sort`, `limit`, and
`fields` control the returned list. Its child `ops` can compute facets and
metrics over the complete match set,
regardless of that list's `limit`. Operations can nest further: a facet can
contain another facet or a `top_docs` list for each bucket.

The [Protobuf API reference](../reference/protobuf.md) lists each message and
its fields. The definitions live in
[`protos/luxir_types.proto`](../../protos/luxir_types.proto), the shared model
for the JSON and gRPC APIs. [`protos/luxir.proto`](../../protos/luxir.proto)
defines the services, including `Searcher.Search`. Generated gRPC clients use
the messages directly; HTTP adds conveniences such as query strings and the
root `top_docs` shorthand used below. See the [full request form](#full-request-form)
to see the operation tree explicitly, and the
[gRPC guide](grpc.md#http-json-versus-protobuf-values) for the JSON mappings.

## One result list

The common request is deliberately shallow: a query, optional filters, how
many documents, which fields.

```http
POST /collections/books/_search

{
  "query": "title_t:(dune OR messiah) AND year_i:>=1965",
  "filter": ["stock_i:>0"],
  "limit": 10,
  "get_number": true,
  "get_scores": true,
  "fields": ["id", "title_t", "year_i", "price_f"]
}
```

```json
{
  "found": 2,
  "docs": [
    {
      "id": "b2",
      "title_t": "Dune Messiah",
      "year_i": 1969,
      "price_f": 8.99,
      "_score_": 0.6702168
    },
    {
      "id": "b1",
      "title_t": "Dune",
      "year_i": 1965,
      "price_f": 9.99,
      "_score_": 0.19659248
    }
  ]
}
```

Anywhere a request takes a query, it takes either form: a bare string is the
[Luxir query language](query-language.md), and a JSON object is a
[structured query](query-reference.md). `filter` clauses constrain the match set without contributing
to the score, and an expression string is usually the clearest way to write
one.

| Field | Meaning |
|---|---|
| `query` | One structured query object or an expression string. Omitted: match all. |
| `filter` | Non-scoring queries ANDed with the main query: expression strings, query objects, or `{"query": ..., "except_ops": [...]}` wrappers that hide a filter from named sub-operations. |
| `limit` | Maximum documents returned. Default `10`; `0` for count/analytics only; `-1` for all matches. |
| `offset` | Zero-based rank of the first returned document. Default `0`. |
| `get_number` | Compute and return the exact match count as `found`. |
| `get_scores` | Add `_score_` to every returned document. |
| `fields` | Fields to retrieve. Omitted: every retrievable field. |
| `sort` | One sort clause or a list of them. Absent means relevance order. |
| `batch_size` | Maximum documents in one streaming response batch. |
| `document_format` | `rows` or `columns`; HTTP defaults to rows, gRPC to columns. |
| `ops` | Facets or metrics over this query's complete match set. |

### Counts are exact, and opt-in

`found` is opt-in, and the choice changes how the engine runs. Without
`get_number`, a top-k request uses per-block score bounds and MaxScore-style
skipping to avoid scoring documents that cannot enter the top k. With
`get_number`, the engine counts the whole match set and returns the exact
total, even when you retrieve only a few documents.

For a count plus analytics with no documents at all, set `limit: 0`. No
document fields are loaded:

```http
POST /collections/books/_search

{
  "query": "title_t:dune",
  "limit": 0,
  "get_number": true,
  "ops": {
    "categories": {
      "field_facet": {
        "field": "category_s"
      }
    }
  }
}
```

```json
{
  "found": 3,
  "docs": [],
  "ops": {
    "categories": {
      "buckets": [
        {
          "val": "science-fiction",
          "count": 2
        },
        {
          "val": "classic",
          "count": 1
        }
      ]
    }
  }
}
```

## One request, several results

Add `ops` beside the query when the response should include analytics as well
as documents. Each operation sees every document the query and its filters
match, regardless of `limit`, and every operation runs over the same
consistent view of the index:

```http
POST /collections/books/_search

{
  "query": "title_t:dune",
  "filter": ["stock_i:>0"],
  "limit": 10,
  "get_number": true,
  "fields": ["id","title_t","price_f"],
  "ops": {
    "categories": {
      "field_facet": {"field":"category_s","limit":10}
    },
    "average_price": "avg(price_f)"
  }
}
```

```json
{
  "found": 42,
  "docs": [
    {
      "id": "b1",
      "title_t": "Dune",
      "price_f": 9.99
    }
  ],
  "ops": {
    "categories": {
      "buckets": [
        {
          "val": "science-fiction",
          "count": 31
        },
        {
          "val": "classic",
          "count": 11
        }
      ]
    },
    "average_price": 11.72
  }
}
```

With root query shorthand, `found` and `docs` appear in the HTTP envelope;
the query's sub-operation results appear under `ops` by name. Expression metrics fold `avg`,
`sum`, `min`, or `max` over value expressions and ignore missing document
values. Integer results stay integers; averages and floating-point results are
doubles. An empty metric domain renders as `null` in JSON.

One request returned the documents, the facet counts for a sidebar, and the
total for a header. See [Faceting](faceting.md) for
terms, range, date, nested, and per-bucket operations, and
[Vector Search](vector-search.md#hybrid-search-with-rrf) for a fusion operation
that ranks several sources and carries facets of its own.

For an interactive sidebar, put the user's choices in each facet's
[`selected`](faceting.md#easy-multi-select-with-selected) array. Luxir builds
the selection filters and keeps alternatives visible for multi-select
automatically.

## Query forms

Every query type, `knn` included, is a node that can appear under boolean
clauses, filters, fusion sources, or another query wrapper.

| Query | Example | Notes |
|---|---|---|
| Match all | `{"all":true}` | Constant-scoring all-documents query. |
| Match | `{"match":{"title_t":"kings"}}` | Analyzes text; the object shown is sugar for explicit `field` and `val`. |
| Exact membership | `{"any_of":{"field":"category_s","values":["classic","fiction"]}}` | Whole values; expression form `category_s:=(classic, fiction)`. |
| Exists | `{"exists":{"field":"year_i"}}` | Supplied value exists; expression shorthand is `year_i:*`. |
| Phrase | `{"phrase":{"field":"title_t","text":"way of kings","slop":0}}` | Position-aware; see the [query language](query-language.md#terms-and-phrases) for slop semantics. |
| Range | `{"range":{"field":"year_i","gte":1960,"lt":1970}}` | Numeric, date, string, ID, or text term ranges. |
| Prefix | `{"prefix":{"field":"title_t","prefix":"king"}}` | Term prefix, not a general wildcard. |
| Fuzzy | `{"fuzzy":{"field":"author_s","term":"Sandersen","max_edits":1}}` | Closest-term rewrite; `max_expansions` defaults to `50`. |
| Boolean | `{"boolean":{"required":[...],"filter":[...],"optional":[...],"prohibited":[...],"min_match":1}}` | Uniform composition of scoring and non-scoring clauses. |
| Constant score | `{"constant_score":{"query":...,"score":1.0}}` | Replaces child scores without changing its match set. |
| Boost | `{"boost":{"query":...,"boost":2.0}}` | Multiplies child scores. A numeric `boost` sibling is input sugar. |
| Simple query | `{"simple_query":{"q":"dune | messiah","fields":["title_t"]}}` | Never-failing syntax for raw search-box input. |
| Expression | `"title_t:dune AND year_i:>=1965"` | Strict developer syntax with exact parse errors and safe `$vars`. |
| kNN | `{"knn":{"field":"embedding_v","query":[...],"k":20}}` | Exact or ANN vector search; see [Vector Search](vector-search.md). |
| Geo box | `{"geo_box":{"field":"location","min_lat":40,"max_lat":42,"min_lon":-75,"max_lon":-72}}` | Inclusive box, including dateline-crossing boxes. |
| Geo distance | `{"geo_distance":{"field":"location","lat":40.71,"lon":-74.01,"radius_meters":5000}}` | Inclusive great-circle radius. |

`match` accepts `operator: "and"` or a numeric `min_match` when analysis
produces several terms. Boolean `optional` clauses rank but do not constrain a
query that already has `required` or `filter` clauses unless `min_match` is set.

There are two query types for text. Text a person typed goes through
[`simple_query`](query-reference.md#simple-query), which never fails to
parse. Text your application authored uses the strict
[query language](query-language.md), which reports parse errors with a byte
offset; its `$vars` are substituted as values rather than syntax, so user
input cannot inject operators. The
[structured query reference](query-reference.md) documents every field and
default.

### Field bindings

A bare field name selects a representation by operation: text operations
(match, phrase, prefix, fuzzy, simple query, expression terms) use the
field's `search` binding, and value operations (`any_of`, ranges, facets,
sorts, metrics) use its `value` binding. `f__label` selects a variant
exactly and `f__self` forces the primary. The rules live in
[Schema: default bindings](schema.md#default-bindings).

The [author example](documents.md#field-variants) shows word search and
whole-name facets using `author_name`. To retrieve specific representations,
name their selectors in `fields`. This request returns the first Asher book:

```http
POST /collections/authors/_search

{
  "query": "author_name:Asher",
  "fields": ["title_t", "author_name", "author_name__s", "author_name__self"],
  "get_number": true,
  "sort": ["author_name", "id"],
  "limit": 1
}
```

```json
{
  "found": 3,
  "docs": [
    {
      "title_t": "Gridlinked",
      "author_name": "Neal Asher",
      "author_name__s": "Neal Asher",
      "author_name__self": "Neal Asher"
    }
  ]
}
```

For an exact whole-author filter, use `any_of`:

```http
POST /collections/authors/_search

{
  "query": {
    "all": true
  },
  "fields": ["id"],
  "get_number": true,
  "filter": [
    {
      "any_of": {
        "field": "author_name",
        "values": ["Neal Asher"]
      }
    }
  ],
  "sort": "id"
}
```

This returns Asher's three books (`b1`, `b2`, and `b4`). A `match` filter for
`Neal` analyzes text and returns all six books. `any_of` on a TEXT
representation is exact token membership: `author_name__self` with `"Neal!"`
looks up the single analyzed term `neal`. See
[`any_of`](query-reference.md#exact-membership-any_of) for the full rules.

## Sorting

Sort by a column field, the query score, or an expression over both:

```http
POST /collections/books/_search

{"query": {"all":true}, "sort": "year_i desc", "fields": ["id","title_t","year_i"]}
```

```json
{
  "docs": [
    {
      "id": "b4",
      "title_t": "Neuromancer",
      "year_i": 1984
    },
    {
      "id": "b3",
      "title_t": "Children of Dune",
      "year_i": 1976
    },
    {
      "id": "b2",
      "title_t": "Dune Messiah",
      "year_i": 1969
    },
    {
      "id": "b1",
      "title_t": "Dune",
      "year_i": 1965
    }
  ]
}
```

`sort` takes one clause or a list of clauses, so `["category_s", "year_i desc"]`
sorts by category and then by year within it. A clause is a string, `expr`,
`expr asc`, or `expr desc`, or an object with the same `expr` and `dir` plus a
`vars` map for `$name` values. The expression is a bare field name or a
numeric value expression:

```json
{
  "sort": {"expr": "popularity_i + score * $weight", "vars": {"weight": 0.25}, "dir": "desc"}
}
```

An omitted direction is descending for `score` (also spelled `_score_`) and
ascending for everything else. `_docid_` sorts by reader-local
`(segment, docid)` order, and after all explicit keys tie, that order is the
final deterministic tiebreak. Documents missing the sort value sort last under
both directions. Responses and `?explain=request` show the canonical
list-of-objects form.

### Sort expressions in detail

Value expressions support numeric constants, `$name` values from the clause's
`vars` map, the reserved `score` leaf, parentheses, and normal arithmetic
precedence: `*` and `/` bind more tightly than `+` and `-`, and unary `-` binds
to its following value. The function spellings remain available:

- Arithmetic: `add`, `sub`, `mul`, and `div`.
- Defaults: `def(value, fallback)` substitutes only when `value` is missing.
- Unary math: `neg`, `abs`, `sqrt`, `log`, `log1p`, and `floor`.
- Multi-valued reducers: `min`, `max`, `avg`, `sum`, and `count`. Missing or
  empty arrays reduce to missing except for `count`, which returns zero (and
  one or zero for a present or missing scalar). Integer `sum` overflow makes
  that document's value missing; `sum` rejects DATE arrays. A composed
  array-valued root must use an explicit reducer. The two-argument `min` and
  `max` compare scalars, which is useful for clamping.

Field names use the value binding. Numeric, date, string, and ID fields keep
the direct column-sort path. Use `col("name")` when a field name is reserved
or is not an identifier. To sort analyzed text, use a string variant.

A bare multi-valued string field sorts by its smallest value ascending and its
largest value descending (string values are stored as a per-document sorted
set, so either edge is free to read). A bare multi-valued numeric field sorts
by its first stored value, the one key that needs no per-document scan; use
`min(f)` or `max(f)` for edge semantics when the scan is worth paying.

Integer-only addition, subtraction, and multiplication stay int64; a double
operand promotes that operation to double, and division always returns double.
A zero denominator makes that value missing rather than failing the request;
`def(value, fallback)` opts it back in. For an array division only the
zero-denominator elements are absent, and reducers use the quotients that are
present. Array arithmetic permits scalar broadcasting but does not implicitly
zip two arrays. A NaN read from a stored column is missing; stored infinities
remain values, including for `min` and `max`; a non-finite result produced by
arithmetic, an invalid math domain, or int64 overflow is an evaluation error.

DATE values keep their DATE meaning through `def`, `min`, `max`, `avg`, and
DATE plus or minus a number. DATE minus DATE produces a number.
Multiplication, division, unary minus, and unary math such as `floor` demote a
DATE to a number (`floor(when_dt / 86400000)` is an epoch-day number); adding
two DATE values or subtracting a DATE from a number is rejected.

A rescore expression must produce a finite value for every matched document.
Handle missing values with `def()` or exclude them with an `exists()` child
query.

## Field retrieval and result shape

`fields` projects stored or column-backed values into each hit. HTTP defaults
to row documents: a document without a requested field omits the key rather
than returning `null`. Set `document_format: "columns"`
when consumers prefer every requested key in every row, with missing cells
rendered as `null`.

With no `fields`, every retrievable logical field comes back: stored text,
string, numeric, date, and `id` values, discovered from the index itself (so
dynamic suffix fields appear under their concrete names), `id` first and the
rest in name order. Vector fields and engine fields such as `_version_` are
returned only when named. A `fields` entry containing `*` is a wildcard
pattern (`"attr_*"`, `"*_s"`, `"t*s"`) that expands through the same
discovery. Discovered fields are always placed in row documents, whatever
`document_format` says; naming fields produces dense columns. A field both
named and matched by a pattern is
returned once, in its explicit placement, so `"fields": ["id", "*"]` returns
an `id` column beside rows of everything else. `_score_` is requested
explicitly (`get_scores`) and keeps the format's placement.

With [field variants](schema.md#field-variants): bare names return the
primary's stored source or its own column, independently of the search and
value bindings. `author_name__s` returns that representation's value under
the key `author_name__s`; `author_name__self` returns the primary under that
key. Output keys deduplicate, not physical sources, so `author_name` and
`author_name__self` can both appear. Patterns without `__` discover logical
names only (`author_name*`). A pattern containing `__` expands over variants
that have a column, under their physical names: `author_name__*` returns
`author_name__s`, and `*__s` returns every `s` variant in the index.
TEXT variants are skipped; retrieve their logical primary for source text.
Name `__self` explicitly to retrieve it. A multi-valued string variant returns
a sorted, deduplicated set while the stored primary retains source order and
duplicates.

Over gRPC the response is a typed `DocList`: dense columns and row maps can
coexist, `row_count` is authoritative, and `_score_` is a synthetic float
column when requested.

## Full request form

The shorthand above is one `top_docs` operation named `q`, and its `ops` are
that operation's sub-operations. The full form spells this out, naming every
operation in a request-level `ops` map:

```json
{
  "ops": {
    "q": {
      "top_docs": {
        "query": "title_t:dune",
        "filter": ["stock_i:>0"],
        "limit": 10,
        "get_number": true,
        "fields": ["id","title_t","price_f"],
        "ops": {
          "categories": {"field_facet": {"field":"category_s","limit":10}},
          "average_price": "avg(price_f)"
        }
      }
    }
  }
}
```

The full form preserves every operation's name and nesting in the response.
Here the documents are at `ops.q.docs`, the count at `ops.q.found`, and the
facet and metric at `ops.q.ops.categories` and `ops.q.ops.average_price`.
Even a single named operation stays under `ops`, including one named `q`.
Each streaming batch preserves these same result paths.

Use the full form when a request needs several independent operations, such
as a `fusion` beside a `top_docs`, two result lists, or a facet over the whole
collection with no query. Any top-document key at the root selects the
shorthand, and `ops` then holds that query's sub-operations rather than
request operations. Only shorthand unwraps its implicit `q` result into
top-level `found`, `docs`, and `ops`.

`?explain=request` echoes the full form of any request. Posting that echo
back executes the same query; an expanded shorthand request now returns
its results under `ops.q`.

## URL request-field overlay

A search can be expressed entirely in a URL, so it can be bookmarked or
shared as a link:

```http
GET /collections/books/_search?query=title_t:dune&limit=10&fields=id,title_t
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
    },
    {
      "id": "b3",
      "title_t": "Children of Dune"
    }
  ]
}
```

GET has no request body. POST accepts the usual JSON body and the same URL
parameters as an overlay; a recognized URL field wins over the corresponding
body field, which makes a saved POST request easy to adjust from a link.

The recognized request-level parameters are:

| Parameter | Value |
|---|---|
| `request_id` | String. |
| `freshness_ms` | Unsigned 64-bit decimal integer. |
| `time_zone` | String; validated by the search engine like the body field. |
| `profile` | Exactly `true` or `false`. |
| `max_parallel` | Signed 32-bit decimal integer; supported modes are validated by the search engine. |

The recognized top-document parameters are:

| Parameter | Value |
|---|---|
| `query` | Expression query string, exactly like a JSON string in the body `query` position. |
| `limit`, `offset` | Signed 64-bit decimal integer. |
| `fields` | Comma-separated field names. An empty value clears the body list. |
| `sort` | One sort clause per parameter, in the body's string form: `expr`, `expr asc`, or `expr desc`. Repeat to form an ordered sort list. |
| `batch_size` | Signed 32-bit decimal integer. |
| `document_format` | Exactly `default`, `rows`, or `columns`. |
| `get_number`, `get_scores` | Exactly `true` or `false`. |

URL form decoding happens before these grammars are applied, so `+` in a
`sort` value is a space. `sort=` clears the body sort list; empty and non-empty
`sort` occurrences cannot be mixed. Scalar parameters and `fields` use their
last occurrence; repeated `sort` parameters accumulate in URL order.

Top-document URL parameters target the `top_docs` operation named `q`, creating
it if the body has no operations. Request-level URL parameters apply to every
operation shape.

Unknown URL parameters are ignored, allowing middleware metadata.
`format=docs` controls response framing and is not a request-field overlay.
URL overlays preserve the body's response shape: updating an explicit
`ops.q` keeps its result under `ops.q`. A query created entirely from URL
parameters uses the shorthand response shape.

`explain=request` returns the effective request, overlay included, as a body
that can be posted back for identical results; `explain=resolved` also shows
field bindings. See [HTTP explain modes](http-api.md#explain-modes).

## Request-level controls

Request-wide settings mix directly with query keys in the shorthand, and sit
beside `ops` in the full form:

```json
{
  "request_id": "search-42",
  "freshness_ms": 50,
  "time_zone": "America/Denver",
  "profile": true,
  "max_parallel": 1,
  "query": "published_dt:>=NOW/DAY",
  "limit": 10
}
```

- `request_id` is an opaque correlation value, echoed in the response envelope
  over HTTP and on every gRPC response within a bidirectional stream.
- `freshness_ms` bounds how stale an index view may be; `0` requires the
  latest commit.
- `time_zone` sets the default time zone for date queries and facets in the
  request. See [Dates and time zones](dates.md).
- `max_parallel` chooses where and how the request runs. `0`, the default,
  executes serially on the transport thread that received it, with no
  scheduler handoff. `1` moves the request onto the
  shared work-stealing scheduler, still serial, so an expensive query does
  not occupy its connection thread. `-1` removes the cap and lets the request
  parallelize across segments and operations. The path collection overrides
  any collection target in the HTTP body.
- `profile: true` returns an execution profile on the final response.

Profiling is opt-in and request-wide, and operations adopt instrumentation
independently; string field facets are the currently instrumented operation.
Their profile contains one entry per executed segment:

```json
{
  "profile": {
    "ops": [
      {
        "name": "categories",
        "pieces": [
          {
            "kind": "segment",
            "segment": 0,
            "max_doc": 100000,
            "strategy": "skinny",
            "cardinality": 12000,
            "domain_size": 84000,
            "thread_id": 123,
            "elapsed_us": 714,
            "details": [
              "seg maxOrd=12000 ords=identity",
              "bitset domain, adaptive point/bulk ord loads"
            ]
          }
        ]
      }
    ]
  }
}
```

`strategy` names the counter representation the counts land in; `details`
says how the segment was counted. Treat `strategy` and the typed numeric
fields as diagnostics, not a stable performance promise, and treat `details`
as prose for humans: its wording, ordering, and entry count change with the
engine, so do not build tooling on it. Serial execution (the default, or
`max_parallel: 1`) keeps parallel scheduling out of segment-timing
comparisons.

## Streaming HTTP responses

The normal HTTP response is chunked NDJSON. Each line is a complete response
envelope for one batch, and `more: true` says that at least one more line is
coming. For small default-limit requests there is normally one line, which is
why it also looks like an ordinary JSON response to simple tools.

`batch_size` places an upper bound on documents in one engine batch; the
server may choose a smaller batch to protect memory. Producers pause when the
connection's buffered output crosses its configured high-water mark, so a slow
reader applies backpressure instead of accumulating the result in RAM.

### Stream every match

Add `?format=docs` and set `limit: -1` when the consumer wants documents
rather than response envelopes. Every match streams over one connection;
there is no cursor, scroll state, or page size to manage:

```http
POST /collections/books/_search?format=docs

{"query": {"all": true}, "limit": -1, "fields": ["id", "title_t"], "get_number": true}
```

```jsonl
{"_header_":{"found":4}}
{"id":"b1","title_t":"Dune"}
{"id":"b2","title_t":"Dune Messiah"}
{"id":"b3","title_t":"Children of Dune"}
{"id":"b4","title_t":"Neuromancer"}
```

Each line is one bare document. If `get_number` or warnings are present, a
leading `_header_` record carries them. Multi-operation document streams use
header markers to name the following operation. The output pipes directly into
the [NDJSON update endpoint](indexing.md#unbounded-ndjson-ingest), which
recognizes and skips header lines.

A clean HTTP chunk terminator means the result set completed; a failure after
output began aborts the chunked body so a conforming client sees truncation
rather than a plausible partial success.

Docs format uses row documents and accepts only `top_docs` and fusion
operations without nested operations.

## Warnings and errors

Warnings report adjustments or recoverable problems in a successful request.
For example, simple-query fuzzy syntax clamps an edit distance above the
supported maximum:

```json
{
  "warnings": [
    {
      "code": "fuzzy_clamped",
      "message": "fuzzy edit distance 5 clamped to the supported maximum of 2"
    }
  ]
}
```

Treat warning `code` as the machine key and `message` as human detail. The
fuzzy query expansion limits are execution policy and do not produce response
warnings.

See [HTTP API conventions](http-api.md#errors) for error responses and how to
handle a search that fails after streaming has begun.

Before execution, `?explain=request` returns the canonical request that the
server parsed; posting that body back executes the same request. This expands
JSON shorthand, but it is not a post-analysis query plan. `?explain=resolved`
returns `{"request": ..., "resolved_fields": [...]}` and runs ordinary
preparation without collecting results. See
[HTTP explain modes](http-api.md#explain-modes).

## Limits

- Ordered page-after pagination is not implemented. `offset` skips the first
  matching documents in ranked order, and collection retains up to
  `offset + limit` documents, so deep pages cost more. An offset past the
  matches returns one empty final batch. Use document-per-line export when the
  goal is to consume every match.
- The `(segment, docid)` tiebreak is reader-local and can change after segment
  merges, so it is not a durable pagination token.
- `max_parallel` supports `-1`, `0`, and `1`; higher values are reserved.
- Only string field facets emit profile entries; other operations run
  unprofiled under `profile: true`.
