# Searching

A Luxir search request describes the result, not a sequence of calls. The
small form asks for one ranked document list, with facets or metrics nested
under the query whose match set they should consume. The full form names
several independent operations. Both are the same model: the small form
becomes a `top_docs` operation named `q`.

HTTP searches accept either method:

```
GET  /collections/{collection}/_search?<request fields>
POST /collections/{collection}/_search
```

GET has no request body. POST accepts the usual JSON body and the same URL
request-field overlay as GET. A recognized URL field wins over the corresponding
body field, which makes saved POST requests easy to adjust from a link.

## URL request-field overlay

This is a complete browser-usable search:

```http
GET /collections/books/_search?query=title_t:dune&limit=10&fields=id,title_t
```

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
| `fields` | Comma-separated field names. An empty value clears the body list; an empty list item is an error. |
| `sort` | One sort clause per parameter: `expr`, `expr asc`, or `expr desc`. Repeat to form an ordered sort list. |
| `batch_size` | Signed 32-bit decimal integer. |
| `document_format` | Exactly `default`, `rows`, or `columns`. |
| `get_number`, `get_scores` | Exactly `true` or `false`. |

URL form decoding happens before these grammars are applied, so `+` in a
`sort` value is a space. `sort=` clears the body sort list. Empty and non-empty
`sort` occurrences cannot be mixed. Scalar parameters and `fields` use their
last occurrence; repeated `sort` parameters accumulate in URL order.

Top-document URL parameters target the operation named `q`. If the body has no
operations, that operation is created. If the body has operations but its
last `q` is not a `top_docs` operation, the request is rejected instead of
guessing another target. Request-level URL parameters apply to every operation
shape.

Unknown URL parameters are accepted and ignored as an open channel for
middleware metadata. Recognized parameters reject lexically invalid values.
The existing `format=docs` parameter controls response framing and is not a
request-field overlay. Add `explain=request` to return the effective request,
including the overlay, as a body that can be posted back for identical results.
Use `explain=resolved` to also inspect field bindings; it runs ordinary request
preparation. See [HTTP explain modes](http-api.md#explain-modes).

## One result list

The common request is deliberately shallow:

```http
POST /collections/books/_search
Content-Type: application/json

{
  "query": "title_t:(dune OR messiah) AND year_i:>=1965",
  "filter": [
    "stock_i:>0"
  ],
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
    {"id":"b1","title_t":"Dune","year_i":1965,"price_f":9.99,"_score_":0.73},
    {"id":"b2","title_t":"Dune Messiah","year_i":1969,"price_f":8.99,"_score_":0.61}
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
| `offset` | Zero-based rank of the first returned document. Default `0`; must be nonnegative. |
| `get_number` | Compute and return the exact match count as `found`. |
| `get_scores` | Add `_score_` to every returned document. |
| `fields` | Fields to retrieve. Omitted: every retrievable field (see below). |
| `sorts` | Value expressions used as sort keys. No list means relevance order. |
| `batch_size` | Maximum documents in one streaming response batch. |
| `document_format` | `rows` or `columns`; HTTP defaults to rows, gRPC to columns. |
| `ops` | Facets or metrics over this query's complete match domain. |

`offset` skips the first matching documents in ranked order, then returns up to
`limit` documents. Each response batch's `offset` is the absolute rank of its
first row; `found` remains the total match count. An offset past the matches
returns one empty final batch. `limit: -1` returns all remaining matches and
`limit: 0` returns no rows. Collection retains up to `offset + limit` documents,
so deep pages cost more. Ordered page-after pagination is not implemented;
use document-per-line export when the goal is to consume every match.

## Query forms

Every query is one node and every node can appear under boolean clauses,
filters, fusion sources, or another query wrapper.

| Query | Example | Notes |
|---|---|---|
| Match all | `{"all":true}` | Constant-scoring all-documents query. |
| Match | `{"match":{"title_t":"kings"}}` | Analyzes text; the object shown is sugar for explicit `field` and `val`. |
| Exact membership | `{"any_of":{"field":"category_s","values":["classic","fiction"]}}` | Uses the value binding; expression form `category_s:=(classic, fiction)`. |
| Exists | `{"exists":{"field":"year_i"}}` | Supplied value exists; expression shorthand is `year_i:*`. |
| Phrase | `{"phrase":{"field":"title_t","text":"way of kings","slop":0}}` | Position-aware; see the [query language](query-language.md#terms-and-phrases) for slop semantics. |
| Range | `{"range":{"field":"year_i","gte":1960,"lt":1970}}` | Numeric, date, string, ID, or text term ranges. |
| Prefix | `{"prefix":{"field":"title_t","prefix":"king"}}` | Term prefix, not a general wildcard. |
| Fuzzy | `{"fuzzy":{"field":"author_s","term":"Sandersen","max_edits":1}}` | Closest-term rewrite; `max_expansions` defaults to `50`. |
| Boolean | `{"boolean":{"required":[...],"filter":[...],"optional":[...],"prohibited":[...],"min_match":1}}` | Uniform composition of scoring and non-scoring clauses. |
| Constant score | `{"constant_score":{"query":...,"score":1.0}}` | Replaces child scores without changing its match set. |
| Boost | `{"boost":{"query":...,"boost":2.0}}` | Multiplies child scores. A numeric `boost` sibling is input sugar. |
| Simple query | `{"simple_query":{"q":"dune | messiah","fields":["title_t"]}}` | Never-failing syntax for raw search-box input. |
| Expression | `"title_t:dune AND year_i:>=1965"` | Strict developer syntax with exact parse errors and safe variables. |
| kNN | `{"knn":{"field":"embedding_v","query":[...],"k":20}}` | Exact or ANN vector search; see [Vector Search](vector-search.md). |
| Geo box | `{"geo_box":{"field":"location","min_lat":40,"max_lat":42,"min_lon":-75,"max_lon":-72}}` | Inclusive box, including dateline-crossing boxes. |
| Geo distance | `{"geo_distance":{"field":"location","lat":40.71,"lon":-74.01,"radius_meters":5000}}` | Inclusive great-circle radius. |

`match` accepts `operator: "and"` or a numeric `min_match` when analysis
produces several terms. Boolean `optional` clauses rank but do not constrain a
query that already has `required` or `filter` clauses unless `min_match` is set.

The [structured query reference](query-reference.md) documents every field and
default. Use [`simple_query`](query-reference.md#simple-query) for text a person
typed and the strict [Luxir query language](query-language.md) for text your
application authored. That separation is intentional: one degrades rather than
fail; the other would rather report the exact byte offset of a bug than guess.

### Field bindings

A bare logical field name selects a representation by operation. Both bindings
default to `self` unless the [schema](schema.md#default-bindings) sets them:

| Operation | Representation |
|---|---|
| Match, phrase, simple query, expression terms/phrases; prefix, fuzzy, wildcard, regex | `search`. A match inside a filter uses the same binding. |
| `any_of` / `:=`, ranges, field/range facets, sort, column expressions, metrics | `value`, with the operation's normal type and capability checks. |
| Retrieval | Primary source store, else its own typed column. |
| Exists (`f:*`), kNN, geo | Primary physical field. |

`f__label` selects exactly that variant; `f__self` forces the primary. Neither
form consults the defaults or tries a sibling if the operation cannot use it.
Exists on a bare name tests primary presence; an explicit selector tests that
representation, which can differ for documents indexed before it was added.

Using the `names` collection from [Schema](schema.md#field-variants), search
the author's words and sort by the normalized whole value:

```http
POST /collections/names/_search

{
  "query": {"match":{"author":"Guin"}},
  "fields": ["id","author","author__s","author__self"],
  "get_number": true,
  "sorts": [{"expr":"author"},{"expr":"id"}]
}
```

```json
{
  "found": 2,
  "docs": [
    {"id":"b3","author":"LE GUIN","author__s":"le guin","author__self":"LE GUIN"},
    {"id":"b1","author":"Ursula K. Le Guin","author__s":"ursula k. le guin","author__self":"Ursula K. Le Guin"}
  ]
}
```

For an exact whole-author filter, use `any_of`:

```http
POST /collections/names/_search

{
  "query": {"all":true},
  "fields": ["id"],
  "get_number": true,
  "filter": [{"any_of":{"field":"author","values":["URSULA K. LE GUIN"]}}]
}
```

This returns only `b1`. A `match` filter for `Guin` still analyzes text and
returns `b1` and `b3`. `any_of` on TEXT is exact token membership: lookup uses
the single analyzed term, so `author__self` with `"Guin!"` looks up `guin` and
finds the same documents as a match query for `Guin!`. A literal producing zero
terms matches nothing. Several terms are an error suggesting match, phrase,
or a whole-value string variant.

Indexed STRING values, analyzed TEXT tokens, and IDs default to
`long_terms: "hash128"` (previously `truncate`). After normalization/analysis,
terms over 255 bytes become a UTF-8-safe prefix of at most 230 bytes plus 25
base36 hash characters; shorter terms stay unchanged. Exact queries, range
bounds, and facet selections apply the same transform. Sorts and ranges retain
source order only up to the prefix; the hash suffix determines the rest.

Prefixes longer than the kept prefix return a superset by falling back to that
prefix. Wildcard and regex queries also fall back when their common leading
literal prefix exceeds the limit; other patterns see stored term bytes.
`truncate` restores the old shared-prefix merges; `reject` fails documents and
over-limit query terms. Policy edits do not validate or rewrite existing terms;
use a new field or variant label and reindex to change the policy safely.
Hashing is not attack-resistant. Facets and term enumeration return stored hash
terms; a stored source still returns the full value. Column-only strings are unlimited.
See [term-space limits](documents.md#ids-and-replacement), including when to use
the full source value instead of a returned hash term in a query or selection.

## Counts and top-k work

`found` is opt-in. Without `get_number`, Luxir can use block score bounds and
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

With no `fields`, every retrievable logical field comes back: stored text, string,
numeric, date, and `id` values, discovered from the index itself (so dynamic
suffix fields appear under their concrete names), `id` first and the rest in
name order. Vector fields and engine fields such as `_version_` are returned
only when named. Discovered fields are always placed in row documents, whatever
`document_format` says; naming fields is what produces dense columns. `_score_`
is requested explicitly (`get_scores`), so it keeps the format's placement: a
dense column under `columns`, a row key under `rows`.

A `fields` entry containing `*` is a wildcard pattern (`"attr_*"`,
`"*_s"`, `"t*s"`). It expands through the same discovery as the empty case:
matching retrievable fields come back as row documents, vectors and engine
fields never match, and a pattern matching nothing is not an error. A field
also named explicitly is returned once, in its explicit placement, so
`"fields": ["id", "*"]` returns an `id` column beside rows of everything
else.

Bare names return the primary's stored source or its own column, independently
of the search/value bindings. `author__s` returns that representation's value
under the key `author__s`; `author__self` returns the primary under that key,
including its stored source. Output keys deduplicate, not physical sources,
so `author` and `author__self` can both appear.

Default discovery and wildcards never expand into variants. `author*` discovers
logical names only, while `author__*` is an error asking for an exact selector
such as `author__s` or `author__self`. A `stored: false` TEXT primary is omitted
from discovery even if its variant has a column. Naming an unretrievable TEXT
representation explicitly is an error; retrieve its logical primary for source
text. A multi-valued string variant returns a sorted, deduplicated set while
the stored primary retains source order and duplicates.

The underlying gRPC response remains a typed `DocList`: dense columns and row
maps can coexist, `row_count` is authoritative, and `_score_` is a synthetic
float column when requested.

## Sorting

Sort a column field explicitly:

```json
{
  "query": {"all":true},
  "sorts": [{"expr":"year_i","dir":"desc"}],
  "fields": ["id","title_t","year_i"]
}
```

The `expr` member accepts either a bare field name or a numeric value expression.
For a bare field sort, `field` is accepted as an input alias; responses and
request echo use the canonical `expr` form.
Numeric, date, string, and ID field names retain the direct column-sort path.
Use `col("name")` when a field name is reserved or is not an identifier. Sorts
and column-expression leaves use the value binding. Analyzed TEXT has no value
column, even when indexed; use a string variant. `author__self` and
`col("author__self")` both fail with a TEXT/no-value-column error in the author
example. `col("author")` still uses `author__s`: `col()` escapes an identifier,
not the binding or capability checks. A string column can sort, but
`min(author)` as a metric fails because it requires a numeric expression.
Documents missing the sort value always sort last, under
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
`vars` map, the reserved `score` leaf, parentheses, and normal arithmetic
precedence. `*` and `/` bind more tightly than `+` and `-`; unary `-` binds to
its following value. The function spellings remain available:

- Arithmetic: `add`, `sub`, `mul`, and `div`.
- Defaults: `def(value, fallback)` substitutes only when `value` is missing.
- Unary math: `neg`, `abs`, `sqrt`, `log`, `log1p`, and `floor`.
- Multi-valued reducers: `min`, `max`, `avg`, `sum`, and `count`. Integer
  `sum` overflow makes that document's value missing; `sum` rejects DATE
  arrays. Missing or empty arrays reduce to missing except for `count`, which
  returns zero. `count` also returns one or zero for a present or missing
  numeric scalar. A composed array-valued root must use an explicit reducer.
  The two-argument `min` and `max` forms compare scalar values and are useful
  for clamping.

For example:

```json
{
  "expr": "popularity_i + score * $weight",
  "vars": {"weight": 0.25},
  "dir": "desc"
}
```

Integer-only addition, subtraction, and multiplication remain int64; a double
operand promotes that operation to double. Division always returns double. A
zero denominator makes that value missing rather than failing the request;
`def(value,fallback)` can opt it back in. For an array division, only the
zero-denominator elements are absent, and reducers use the quotients that are
present. Array arithmetic permits scalar broadcasting but does not implicitly
zip two arrays. A NaN read from a stored numeric column is missing. Stored
infinities remain values, including for `min` and `max`; a non-finite result
produced by arithmetic, an invalid math domain, or int64 overflow remains an
evaluation error.

DATE values retain their DATE meaning through `def`, `min`, `max`, `avg`, and
DATE plus or minus a number. DATE minus DATE produces a number. Multiplication,
division, unary minus, and unary math such as `floor` demote DATE to a number;
adding two DATE values or subtracting a DATE from a number is rejected. For
example, `floor(when_dt / 86400000)` produces an epoch-day number.

A rescore expression is stricter than a sort expression: it must produce a
finite value for every matched document. A missing rescore value fails the
search and names `def()` and an `exists()` child query as the two ways to make
the expression total.

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

Add `ops` beside the query when the response should include analytics as well
as documents. Each operation sees every document the query and its filters
match, regardless of `limit`:

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
  "docs": [{"id":"b1","title_t":"Dune","price_f":9.99}],
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
operation results appear under `ops` by name. Expression metrics fold `avg`,
`sum`, `min`, or `max` over value expressions and ignore missing document
values. Integer results stay typed as integers; averages and floating-point
results are doubles. An empty metric domain renders as `null` in JSON.

See [Faceting](faceting.md) for terms, range, date, nested, and per-bucket
operations. See [Vector Search](vector-search.md#hybrid-search-with-rrf) for a
full fusion operation with several ranked sources.

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

The response is the same. Use the full form when a request needs several
independent operations: a `fusion` beside a `top_docs`, two result lists, or a
facet over the whole collection with no query. Any top-document key at the
root selects the shorthand, and `ops` then holds that query's sub-operations
rather than request operations. `?explain=request` echoes the full form of any
request.

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

`freshness_ms` bounds how stale an index view may be; `0` requires the latest
commit. `time_zone` supplies the civil frame for every date query and facet in
the request. `max_parallel` caps intra-request parallelism, executed on the
server's shared worker pool: `1` executes the request serially on one worker
thread; `-1` removes the cap. `0`, the default, lets the engine choose -
today that is serial execution directly on the transport thread that received
the request (no scheduler handoff at all; cheap queries skip scheduling
entirely, at the cost of occupying that connection's thread for the query's
duration). Set `1` to move a request you expect to be expensive off the
connection thread. Values above `1` are reserved and currently rejected. The
path collection overrides any collection target in the HTTP body.

`request_id` is echoed by gRPC responses for correlation within a bidirectional
stream. The custom HTTP response envelope does not currently include it.

`profile: true` returns an execution profile on the final response. Profiling
is opt-in and request-wide, but operations adopt instrumentation independently;
string field facets are the currently instrumented operation. Their profile
contains one entry per executed segment with the selected counter
representation, selection inputs such as cardinality and domain size, thread
ID, elapsed microseconds, and human-readable details:

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
        "details": ["seg maxOrd=12000 ords=identity",
                    "bitset domain, adaptive point/bulk ord loads"]
      }]
    }]
  }
}
```

`strategy` names the counter representation the counts land in. *How* the
segment was counted is an orthogonal choice reported in `details`: walking the
domain over the field's ord column, walking the domain's complement and
subtracting from each term's docFreq (cheap when the domain covers most of the
segment), or intersecting each term's postings with the domain.
Treat `strategy` and the typed numeric fields as diagnostics, not a stable
performance promise. `details` is prose for humans and is not part of the API:
its wording, ordering, and entry count change with the engine, so do not build
tooling on it. Serial execution (the default, or `max_parallel: 1`) keeps
parallel scheduling out of segment-timing comparisons.

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
curl -s 'http://localhost:9400/collections/books/_search?format=docs' \
  -H 'Content-Type: application/json' \
  -d '{"query":{"all":true},"limit":-1,"fields":["id","title_t"]}'
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

Warnings declare a request that succeeded after recovering from or clamping
user-facing input. For example, simple-query fuzzy syntax clamps an edit
distance above the supported maximum:

```json
{"warnings":[{"code":"fuzzy_clamped","message":"..."}]}
```

Treat warning `code` as the machine key and `message` as human detail. The
fuzzy query expansion limits are execution policy and do not produce response
warnings.

Errors have the same shape plus a `kind`:

```json
{"request_id": "q7", "error": {"kind": "invalid_request", "code": "unknown_field", "message": "Field not found: titel"}}
```

Unknown JSON keys, invalid enums, wrong field types, and expression syntax
errors are rejected before submission with HTTP `400`; a query that fails
after submission returns the same object as the final response line with no
`docs` or `ops`. `kind` fixes the HTTP status and says whose fault it is, and
`code` is the stable key; see [HTTP API conventions](http-api.md#errors) for
the table. Before execution, `?explain=request` returns the canonical request
that the server parsed; posting that body back executes the same request. This
expands JSON shorthand, but it is not a post-analysis query plan.
`?explain=resolved` returns `{"request": ..., "resolved_fields": [...]}` and
runs ordinary preparation without executing result collection. It acquires
readers, respects freshness, and performs semantic validation and query
preparation work. Replay its `request` member; the envelope is not a request
body. See [HTTP explain modes](http-api.md#explain-modes).
