# Structured query reference

Every structured query is a JSON object with exactly one query arm. Query
objects compose uniformly inside boolean clauses, filters, wrappers, kNN
filter domains, and fusion sources. This page is the field-level reference;
[Searching](searching.md) covers result collection and the
[query language](query-language.md) covers the strict expression shorthand.

Unknown arms and unknown fields are errors. Add `?explain=request` to an HTTP
query to see the canonical request after shorthand expansion.

## Match all and exists

```json
{"all":true}
```

`all` matches every live document with a constant score.

```json
{"exists":{"field":"year_i"}}
```

`exists` matches documents that supplied at least one accepted value for the
field. The expression shorthand is `year_i:*`.

## Match

`match` applies the field's query-time conversion and analyzer:

```json
{
  "match": {
    "field": "title_w",
    "val": "dune messiah",
    "operator": "and"
  }
}
```

The HTTP dialect also accepts the field-name form:

```json
{"match":{"title_w":"dune messiah","operator":"and"}}
```

| Field | Meaning |
|---|---|
| `field` | Field to query. Required. |
| `val` | Value to analyze or coerce. Required. |
| `operator` | `or` (default) or `and` for several analyzed terms. |
| `min_match` | Minimum analyzed terms that must match; overrides `operator` and is clamped to the term count. |

Text values are analyzed with the field analyzer. String and ID values are
matched as exact terms. Numeric and date values are coerced through the same
rules as indexing and matched through their columns.

## Boolean

```json
{
  "boolean": {
    "required": [
      {"match":{"title_w":"dune"}}
    ],
    "filter": [
      {"range":{"field":"year_i","gte":1960}}
    ],
    "optional": [
      {"match":{"author_s":"Herbert"}}
    ],
    "prohibited": [
      {"match":{"status_s":"withdrawn"}}
    ],
    "min_match": 0
  }
}
```

| Field | Meaning |
|---|---|
| `required` | Must match and contributes to score. |
| `filter` | Must match without contributing to score. |
| `optional` | Contributes to score; constrains only an optional-only boolean unless `min_match` says otherwise. |
| `prohibited` | Must not match. |
| `min_match` | Required number of optional clauses. `0` uses the contextual default. |

With any `required` or `filter` clause, optional clauses rank coincident hits
but do not constrain the match set by default. With only optional clauses, at
least one must match. Set `min_match` to make the optional group an explicit
constraint.

Top-level `top_docs.filter` accepts bare query strings and structured query
objects. A routing wrapper adds `except_ops` when sibling operations should not
see the filter:

```json
"filter":[
  {"match":{"status_s":"active"}},
  {"query":"brand_s:acme","except_ops":["brands"]}
]
```

Routing wrappers are accepted by the JSON and protobuf APIs but currently fail
validation until routed filter execution is implemented.

## Phrase

The common form analyzes one text value:

```json
{"phrase":{"field":"title_w","text":"left hand","slop":0}}
```

Set exactly one input form:

| Field | Meaning |
|---|---|
| `text` | One unanalyzed string; the field analyzer produces the phrase terms and positions. |
| `words` | Unanalyzed word strings, each passed through analysis. |
| `terms` | Already analyzed string terms. |
| `terms_bin` | Already analyzed byte terms for typed protobuf callers. |
| `positions` | Optional explicit positions aligned with `terms`/`terms_bin`. |
| `slop` | Maximum spread of query-adjusted positions. Default `0`; must be non-negative. |

Slop is not simply the number of intervening words. An adjacent transposition
costs `2`, and multi-valued text fields have a position gap of `100`. See
[Query language: terms and phrases](query-language.md#terms-and-phrases).

## Prefix

```json
{"prefix":{"field":"title_w","prefix":"mess"}}
```

`prefix` is an indexed-term prefix, not a wildcard expression. Text fields
apply their multi-term normalization/folding but do not tokenize it; string and
ID fields use the bytes verbatim. An empty prefix matches documents with at
least one indexed term for the field.

## Fuzzy

```json
{
  "fuzzy": {
    "field": "author_s",
    "term": "herbet",
    "max_edits": 1,
    "prefix_length": 1,
    "max_expansions": 0
  }
}
```

| Field | Meaning |
|---|---|
| `field` | Indexed term field. Required. |
| `term` | Term to compare. Text fields normalize/fold but do not tokenize it; string and ID fields use it verbatim. Required. |
| `max_edits` | Byte-wise Levenshtein distance from `0` to `2`. Unset uses `0` for terms of at most 2 bytes, `1` through 5 bytes, then `2`. |
| `prefix_length` | Leading bytes that must match exactly. Unset defaults to `1`; explicit `0` disables it. |
| `max_expansions` | Maximum term expansions, closest first. Unset or `0` uses the default `50`; a positive value pins the requested cap. |

The engine also applies a current clause budget of `64` and may apply a lower
operator limit. Expansion truncation is silent execution policy; the
dictionary-dependent result does not change response metadata. Scoring uses
blended BM25 statistics; filter context is constant-scoring, but both use the
same expansion set and match set. Simple-query syntax still warns when it
clamps an unsupported edit distance before constructing the fuzzy query.

## Range

```json
{"range":{"field":"year_i","gte":1960,"lt":1970}}
```

Use at most one lower bound (`gte` or `gt`) and one upper bound (`lte` or
`lt`). Omit one side for an open range. With no bounds, the range becomes an
exists query.

Numeric and date fields compare column values. Date bounds accept epoch
milliseconds, ISO-8601, partial dates, and date math; see
[Dates and time zones](dates.md). String, ID, and text fields range over indexed
terms in byte order with constant scores. Text bounds receive the field's
normalization/folding but are not tokenized.

## Constant score and boost

Replace every child score while retaining its match set:

```json
{
  "constant_score": {
    "query": {"match":{"status_s":"active"}},
    "score": 1.0
  }
}
```

`score` defaults to `1.0`; explicit `0` is legal.

Multiply child scores without changing matches:

```json
{
  "boost": {
    "query": {"match":{"title_w":"dune"}},
    "boost": 2.0
  }
}
```

`boost` defaults to `1.0` and must be finite and non-negative. HTTP also
accepts a numeric sibling as input sugar:
`{"match":{"title_w":"dune"},"boost":2}`. Canonical output uses the wrapper.

## Simple query

`simple_query` is for raw search-box input whose syntax must never fail:

```json
{
  "simple_query": {
    "q": "dune | messiah -movie",
    "fields": ["title_w","description_t"],
    "operator": "or",
    "min_match": 1,
    "allowed_fields": ["title_w","author_s"]
  }
}
```

| Field | Meaning |
|---|---|
| `q` | Raw user input. |
| `fields` | Required non-empty fields for unfielded terms; scores add across fields. |
| `operator` | Default adjacency: `or` (default) or `and`. |
| `min_match` | Minimum top-level optional clauses when the parsed shape can apply it. |
| `allowed_fields` | Optional allowlist for `field:value`; empty permits every queryable schema field. |

The never-failing promise applies to the contents of `q`, not to the request
envelope: an empty `fields` list or an unknown schema field is still a request
error. Constructs that cannot be honored degrade to literal terms or produce a
declared search warning. The [Quickstart](quickstart.md#forgiving-end-user-search)
shows the intended search-box use.

## Expression

The structured expression arm carries strict developer-authored syntax and
injection-safe values:

```json
{
  "expr": {
    "q": "title_w:$title AND year_i:>=$year",
    "vars": {"title":"dune","year":1965}
  }
}
```

A bare JSON string is sugar for `expr.q`. Variables are values and are never
parsed as query syntax. See [Query language](query-language.md) for the full
grammar and the function-call subset.

## Vector and geo

Vector and geographic queries use the same `Query` node but have enough field
and execution semantics to warrant their own guides:

- [`knn`](vector-search.md) covers `field`, `query`, `k`, `refine_candidates`,
  `exact`, and the per-engine `ivf` object (`nprobe`, `min_scan_fraction`).
- [`geo_box` and `geo_distance`](geo-search.md) cover coordinate order,
  inclusive boundaries, dateline crossing, and distance units.

These query types are structured-only. Expression functions cannot currently
represent vector or coordinate-list arguments, so use the objects shown in the
vector and geo guides.
