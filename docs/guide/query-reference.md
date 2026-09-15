# Structured query reference

Every structured query is a JSON object with exactly one query arm, and every
arm is a node that composes anywhere: under boolean clauses, in filters, inside
wrappers, as a kNN filter domain, or as a fusion source. The
[query language](query-language.md) is a string syntax for the same tree.
This page is the field-level reference for the arms. It doubles as the
function reference for
the [query language](query-language.md): a function's name is the arm's JSON
name and its arguments are the arm's fields, so `fuzzy(smith, field=name_s,
max_edits=2)` and the JSON below mean the same thing.
[Queries and search operations](searching.md#queries-and-search-operations)
explains how a query selects and scores documents while operations collect
results, and links the protobuf definitions shared by JSON and gRPC.

```http
POST /collections/books/_search

{"query": {"match": {"title_t": "dune"}}, "fields": ["id"], "get_number": true}
```

```json
{
  "found": 2,
  "docs": [
    {
      "id": "b1"
    },
    {
      "id": "b2"
    }
  ]
}
```

Add `?explain=request` to an HTTP query to see the canonical request after
shorthand expansion.

A bare field name picks a representation by operation: text operations use
the field's `search` binding, value operations (`any_of`, ranges, facets,
sorts, metrics) use its `value` binding, and `f__label` or `f__self` selects
one representation directly. See [default bindings](schema.md#default-bindings)
and the [worked example](searching.md#field-bindings).

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
Bare exists uses primary presence; an explicit selector tests that physical
representation's presence.

## Match

`match` applies the field's query-time conversion and analyzer:

```json
{
  "match": {
    "field": "title_t",
    "val": "Dune Messiah",
    "operator": "and"
  }
}
```

The HTTP dialect also accepts the field-name form:

```json
{"match":{"title_t":"dune messiah","operator":"and"}}
```

| Field | Meaning |
|---|---|
| `field` | Field to query. Required. |
| `val` | Value to analyze or coerce. Required. |
| `operator` | `or` (default) or `and` for several analyzed terms. |
| `min_match` | Minimum analyzed terms that must match; overrides `operator` and is clamped to the term count. |

Text values are analyzed with the resolved field analyzer. String values use
their normalizer, if configured, and match as exact terms. Numeric and date
values are coerced through the same rules as indexing and matched through
their columns. Terms over 255 bytes follow the field's
[`long_terms` policy](schema.md#string-normalization-and-length).

## Exact membership (`any_of`)

`any_of` matches any listed value in the value representation. With the
`authors` collection from [Documents and values](documents.md#field-variants):

```http
POST /collections/authors/_search

{
  "query": {
    "any_of": {
      "field": "author_name",
      "values": ["Neal Asher"]
    }
  },
  "fields": ["id"],
  "get_number": true,
  "sort": "id"
}
```

```json
{
  "found": 3,
  "docs": [
    {
      "id": "b1"
    },
    {
      "id": "b2"
    },
    {
      "id": "b4"
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `field` | Field to query through its value binding, or an explicit selector. Required. |
| `values` | Exact values; any matching value admits the document. |

STRING, TEXT, and ID exact membership requires indexed terms. Numeric/date
representations can use their columns.

STRING literals are whole values and use the same normalizer as ingest. TEXT
literals use exact token membership: each value must analyze to at most one
term. For example, `author_name__self` with `"Neal!"` looks up `neal` and
matches all six books. A value producing zero terms matches nothing. For
multiple terms, use match, phrase, or a whole-value string variant. Literals
over 255 bytes follow the field's
[`long_terms` policy](schema.md#string-normalization-and-length).

The expression forms are `field:=value` and `field:=(v1, v2)`; see
[exact values](query-language.md#exact-values).

## Boolean

```json
{
  "boolean": {
    "required": [
      {"match":{"title_t":"dune"}}
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

## Filters, routing, and domains

The `filter` list of a `top_docs` operation (and of the request shorthand)
holds non-scoring queries ANDed with the main query. Each entry is a bare
expression string, a structured query object, or a routing wrapper that adds
`except_ops` when sibling operations should not see the filter:

```json
"filter":[
  {"match":{"status_s":"active"}},
  {"query":"brand_s:acme","except_ops":["brands"]}
]
```

The filter still restricts the document results, but the named operations in
`TopDocs.ops` run without it: a `brands` facet keeps showing every brand while
the result list is narrowed to one. Names must be distinct, nonempty keys in
that map. Routing is supported by both JSON and protobuf; Fusion filters and
Fusion source filters do not accept `except_ops`.

An operation directly in `SearchRequest.ops` or `TopDocs.ops` may also set a
`domain`:

```json
"brands": {
  "field_facet": {"field":"brand_s"},
  "domain": {"query":"stock_s:yes", "apply_parent_filters":true}
}
```

`domain.query` replaces the incoming document set with that query's matches
across the collection. Without it, the operation inherits its incoming set.
`domain.filter` adds local constraints in either case. With a replacement
query, `apply_parent_filters: true` reapplies the immediate parent's filters,
including facet selections, while respecting `except_ops`. It defaults to
false and requires `domain.query`; it does not restore the parent's query or
ancestor domain. Domain overrides are not supported directly in `Fusion.ops`
or beneath facet buckets.

## Phrase

The common form analyzes one text value:

```json
{"phrase":{"field":"title_t","text":"way of kings","slop":0}}
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
{"prefix":{"field":"title_t","prefix":"mess"}}
```

`prefix` is an indexed-term prefix, not a wildcard expression. Text fields
apply their multi-term normalization/folding but do not tokenize it; STRING
uses its normalizer if present, and ID uses the bytes verbatim. An empty prefix
matches documents with at least one indexed term for the field. A prefix
longer than the kept prefix of a
[long term](schema.md#string-normalization-and-length) falls back to that
prefix and returns a superset.

## Wildcard

```json
{"wildcard":{"field":"title_t","pattern":"du*"}}
```

| Field | Meaning |
|---|---|
| `field` | Indexed term field. Required. |
| `pattern` | `*` matches any bytes, `?` matches one codepoint, `\` escapes the next character. Required. |

The pattern matches an entire indexed term. On TEXT fields, literal characters
fold the way the field folds text, while `*`, `?`, and escapes are syntax and
never fold; STRING fields also normalize literal characters; ID fields use
literals verbatim. Wildcard queries are constant-scoring. A pattern whose
leading literal prefix exceeds the kept prefix of a
[long term](schema.md#string-normalization-and-length) falls back to that
prefix; otherwise the pattern sees the stored term bytes. The expression form
is `wildcard(du*, field=title_t)`.

## Regex

```json
{"regex":{"field":"title_t","pattern":"dune|kings"}}
```

| Field | Meaning |
|---|---|
| `field` | Indexed term field. Required. |
| `pattern` | Anchored whole-term regular expression: `\|`, concatenation, groups, repetition, `.`, and character classes. Required. |

The match is anchored to the whole indexed term, so `mess` does not match
`messiah` but `mess.*` does; `^` and `$` are literal characters. On TEXT
fields, literal characters fold the way the field folds text; character
classes and ranges are codepoint-exact. STRING fields also normalize literal
characters; ID fields use literals verbatim. Regex queries are
constant-scoring and follow the same long-term prefix fallback as wildcards.
The expression form is `regex(dune|kings, field=title_t)`.

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
| `term` | Term to compare. TEXT normalizes/folds without tokenizing; STRING uses its normalizer if present; ID uses it verbatim. Required. |
| `max_edits` | Byte-wise Levenshtein distance from `0` to `2`. Unset uses `0` for terms of at most 2 bytes, `1` through 5 bytes, then `2`. |
| `prefix_length` | Leading bytes that must match exactly. Unset defaults to `1`; explicit `0` disables it. |
| `max_expansions` | Maximum term expansions, closest first. Unset or `0` uses the default `50`; a positive value pins the requested cap. |

The engine also applies a current clause budget of `64` and may apply a lower
operator limit. Truncation is not reported in the response. Scoring uses
blended BM25 statistics; filter context is constant-scoring, but both use the
same expansion set and match set. Simple-query syntax still warns when it
clamps an unsupported edit distance before constructing the fuzzy query.
Distance is measured on stored term bytes, so a
[long term](schema.md#string-normalization-and-length) compares its prefix
plus hash.

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
normalization/folding but are not tokenized; STRING bounds use its normalizer.
The bare field uses the value binding, including a range with no bounds.

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
    "query": {"match":{"title_t":"dune"}},
    "boost": 2.0
  }
}
```

`boost` defaults to `1.0` and must be finite and non-negative. HTTP also
accepts a numeric sibling as input sugar:
`{"match":{"title_t":"dune"},"boost":2}`. Canonical output uses the wrapper.

## Rescore

Keep the child query's matches and replace each hit's score with a value
expression:

```json
{
  "rescore": {
    "query": {"match":{"title_t":"dune"}},
    "expr": "score * $weight + log1p(popularity_i)",
    "vars": {"weight": 10}
  }
}
```

| Field | Meaning |
|---|---|
| `query` | The child query. Its matches are the rescored set. Required. |
| `expr` | A numeric value expression; `score` is the child's score and column names read the document's values. Required. |
| `vars` | Scalar values for `$name` references in `expr`. |

The expression uses the same value-expression language as sorting (arithmetic,
`def`, unary math, and reducers; see [Searching](searching.md#sorting)) and
must produce a finite value for every matched document. Handle missing values
with `def()` or exclude them with an `exists()` clause in the child query.
The expression form is
`rescore(title_t:dune, expr=score * log1p(popularity_i))`, whose `$vars`
bind from the surrounding expression's `vars` map.

## Simple query

`simple_query` is for raw search-box input and never fails to parse:

```json
{
  "simple_query": {
    "q": "dune | messiah -movie",
    "fields": ["title_t","description_t"],
    "operator": "or",
    "min_match": 1,
    "allowed_fields": ["title_t","author_s"]
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

Unsupported syntax in `q` degrades to literal terms or produces a search
warning. The [Quickstart](quickstart.md#forgiving-end-user-search)
shows the intended search-box use.

Expansion fields are resolved through `search` and deduplicated by physical
identity: `author_name` and `author_name__self` from the
[author example](documents.md#field-variants) produce one expansion.
`allowed_fields` restricts exact resolved search targets; allowing one
representation does not grant access to its siblings.

## Expression

The `expr` arm holds a query-language string and its variables:

```json
{
  "expr": {
    "q": "title_t:$title AND year_i:>=$year",
    "vars": {"title":"Dune","year":1965}
  }
}
```

A bare JSON string is sugar for `expr.q`. Variables are substituted as values,
not parsed as query syntax. See [Query language](query-language.md) for the full
grammar and the function-call subset.

## Vector and geo

Vector and geographic queries use the same `Query` node and are documented
in their own guides:

- [`knn`](vector-search.md) covers `field`, `query`, `k`, `refine_candidates`,
  `exact`, and the per-engine `ivf` object (`nprobe`, `min_scan_fraction`).
- [`geo_box` and `geo_distance`](geo-search.md) cover coordinate order,
  inclusive boundaries, dateline crossing, and distance units.

## Limits

- `knn`, `geo_box`, and `geo_distance` are structured-only: expression
  functions cannot currently represent vector or coordinate-list arguments,
  so use the objects shown in the vector and geo guides.
- New structured arms are not automatically callable from the query language
  until their expression behavior is declared.
- `except_ops` routing is not accepted on Fusion filters or Fusion source
  filters, and `domain` overrides are not supported directly in `Fusion.ops`
  or beneath facet buckets.
- Fuzzy expansion truncation (the `64` clause budget and any lower operator
  limit) is not reported in the response.
