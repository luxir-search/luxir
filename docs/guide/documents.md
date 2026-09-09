# Documents and values

A Luxir document is a flat map from field name to typed value. The schema says
how each value is indexed and retained; the document itself carries no type
tags. This page defines the boundary that indexing, querying, and retrieval
share.

```json
{
  "id": "b1",
  "title_t": "The Way of Kings",
  "year_i": 2010,
  "tags_ss": ["fantasy","epic"]
}
```

Nested objects and nested documents are not a field type. Model relationships
with IDs or flatten the fields in the producer.

## Field names choose the contract

With the default schema, a recognized suffix chooses the type:

- `title_t` is analyzed text;
- `category_s` is an indexed exact string;
- `year_i` is an integer column;
- `tags_ss` is a multi-valued exact string;
- `published_dt` is a date column;
- `embedding_v` is a vector column.

An unknown field with no matching suffix is a per-document error, not an
ignored property. Define a concrete field or template through the
[schema API](schema.md) when suffixes are not the desired public names.

A field with [variants](schema.md#field-variants) still takes one input value
(or one array for a multi-valued field). Send `author` once; its primary and
variants each convert that submitted value. Derived names such as `author__s`
and `author__self` are request selectors, never document keys. A final document
key containing `__` is a per-document error.

[`field_map`](indexing.md#field-mapping) may rename an external key containing
`__` onto a logical field, or drop it. It may not target a variant, a `__self`
selector, or an abstract template name; an invalid target fails the whole
request. Mapping is rename-or-drop, and the logical field supplies the variants.

Request objects follow a separate but related rule: unknown request keys are
errors. Luxir does not silently accept a misspelled query option.

## Missing, null, and empty

For every field type, an absent field and an explicit JSON `null` both mean
that the document has no value for that field. The document does not enter the
field's postings or column, `exists` does not match it, and row-format retrieval
omits the key.

An empty array on a multi-valued field likewise supplies no values. Use a
single value for a single-valued field and an array for a field declared
`multi`. For vectors, `[]` supplies no vectors to a multi-valued field but is
an invalid empty vector on a single-valued field. Supplying several values to
a single-valued field is an update error.

Missing values do not acquire a schema default. Search result columns use a
type-specific missing sentinel internally; HTTP column-format output renders
the missing cell as `null`.

## IDs and replacement

`id` is the reserved unique-ID field. String and numeric values are accepted;
numeric IDs use their canonical string rendering, so `123` and `"123"` name
the same ID.

An ID is not currently required. A document with an absent or null ID can be
searched, but it cannot be overwritten or deleted by ID and has no useful
external identity. In ordinary collections, treat `id` as required at the
producer boundary.

IDs are limited to 255 UTF-8-safe bytes in the term space. Longer IDs
are truncated in the term space, so values that share the same first 255-byte
prefix collide for overwrite and delete purposes. Keep IDs within that bound.
STRING fields instead reject values over 255 bytes after normalization,
including column-only strings. A string variant that exceeds this limit fails
the document even if its TEXT primary accepts the source.

With the normal `allow_dups: false`, another document with the same ID replaces
the old document. Replacement is whole-document replacement: fields omitted by
the new version disappear. Luxir does not currently implement field patches.
Set `allow_dups: true` only when duplicate IDs are intentionally append-only.

`_version_` is reserved for internal overwrite ordering and is not an
application field.

## Scalar coercion

Index-time and query-time coercion share one implementation: a literal accepted
for a field at ingest can be used to match or bound that field later.

- String, text, and ID fields accept strings. Numeric and boolean scalars are
  converted to their canonical text rendering.
- Integer fields accept JSON integers, integral floating-point numbers, and
  strings that parse to an integral value. Fractional or out-of-range values
  are errors.
- Float and double fields accept JSON numbers and numeric strings.
- Date fields accept epoch milliseconds, ISO-8601 strings, partial dates, and
  date math. Update messages interpret offset-less date text in UTC; search
  requests may supply a query time zone. See [Dates](dates.md).
- Geo points and vectors have array shapes described in their dedicated guides;
  they are not generic numeric multi-value fields. A vector is a number array;
  integer elements are accepted, while doubles must narrow to finite float32.

Coercion is deliberately not a best-effort parser: trailing garbage, an object
where a scalar is expected, a wrong vector dimension, or an invalid coordinate
is a per-document error.

## Multi-valued shapes

The outer JSON shape follows the schema:

```json
{
  "tags_ss": ["fiction","classic"],
  "years_is": [1965,1969],
  "locations": [[-74.0060,40.7128],[-0.1278,51.5074]],
  "embedding_vs": [[0.8,0.1,0.1],[0.2,0.7,0.1]]
}
```

String, text, numeric, and date multi-fields use an array of scalar values. A
multi-geo field uses an array of `[lon,lat]` points. A multi-vector field uses
an array of number arrays. A bare number array is also accepted as a
one-vector list. The field must be declared `multi: true`; the `_ss`, `_is`,
`_fs`, `_ds`, `_dts`, and `_vs` default suffixes already are.

A multi-valued text field analyzes each input value separately and inserts a
position gap of `100` between values. A phrase therefore does not cross values
unless its slop reaches that gap.

With the `_name` and `_names` templates from
[Schema](schema.md#templates-and-inheritance), one input list supplies both
representations:

```http
POST /collections/names/_update

{
  "docs": [{
    "id":"n1",
    "author_name":"Ursula K. Le Guin",
    "author_names":["Le Guin","LE GUIN","Martin"]
  }],
  "commit": {}
}
```

```http
POST /collections/names/_search

{
  "query": "id:n1",
  "fields": ["author_name","author_name__s","author_names","author_names__s"],
  "get_number": true
}
```

```json
{"found":1,"docs":[{"author_name":"Ursula K. Le Guin","author_name__s":"ursula k. le guin","author_names":["Le Guin","LE GUIN","Martin"],"author_names__s":["le guin","martin"]}]}
```

## What is returned

Columns and stored fields solve different problems:

- A column retains a typed per-document value for sorting, faceting, numeric
  and geo queries, vector search, and retrieval where that type is supported.
- A stored TEXT, STRING, or ID primary retains canonical source text in
  compressed chunks, before analysis or normalization. Numeric and boolean
  inputs become text: a numeric `42` stored as TEXT returns `"42"`. Text
  defaults to stored.

Numeric primaries return their typed column; `stored` is ignored for them.
Using the `names` collection from [Schema](schema.md#field-variants):

```http
POST /collections/names/_search

{"query":"id:b1","fields":["edition","edition__label"],"get_number":true}
```

```json
{"found":1,"docs":[{"edition":42,"edition__label":"0042"}]}
```

The submitted `"0042"` is not preserved by the numeric primary. Each branch
converted the input independently; reindexing needs the producer's input, not
just the primary's retrieved value.

Bare retrieval and `__self` use the primary's stored source when enabled,
otherwise its own column. A normalized STRING primary without storage returns
its normalized column value. Multi-valued string columns are sorted,
deduplicated sets; stored source lists retain order and duplicates. Variants
have no stored copy. An exact selector returns that representation's column
under the selector key; a TEXT variant has no retrievable value.

Omitting `fields` returns retrievable logical fields except vectors and engine
fields; name fields, or use `*` wildcard patterns such as `"attr_*"`, to
project a subset. Wildcards never expand variants; `author__*` is an error.
A `stored: false` TEXT primary is omitted even if a variant can be retrieved (see
[Searching](searching.md#field-retrieval-and-result-shape)). HTTP row format
omits missing fields; HTTP column format includes the requested key with
`null`. Geo columns are a current
exception: they participate in queries but are not yet decoded by document
projection. See [Geo search](geo-search.md#current-limits).

Binary is reserved in the wire/schema enum but is not a usable engine field
type in the current release.
