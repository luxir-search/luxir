# Documents and values

A Solux document is a flat map from field name to typed value. The schema says
how each value is indexed and retained; the document itself carries no type
tags. This page defines the boundary that indexing, querying, and retrieval
share.

```json
{
  "id": "b1",
  "title_w": "the left hand of darkness",
  "year_i": 1969,
  "tags_ss": ["fiction","classic"]
}
```

Nested objects and nested documents are not a field type. Model relationships
with IDs or flatten the fields in the producer.

## Field names choose the contract

With the default schema, a recognized suffix chooses the type:

- `title_w` is analyzed text;
- `category_s` is an indexed exact string;
- `year_i` is an integer column;
- `tags_ss` is a multi-valued exact string;
- `published_dt` is a date column;
- `embedding_v` is a vector column.

An unknown field with no matching suffix is a per-document error, not an
ignored property. Define a concrete field or template through the
[schema API](schema.md) when suffixes are not the desired public names.

Request objects follow a separate but related rule: unknown request keys are
errors. Solux does not silently accept a misspelled query option.

## Missing, null, and empty

For scalar, text, and geo fields, an absent field and an explicit JSON `null`
both mean that the document has no value for that field. The document does not
enter the field's postings or column, `exists` does not match it, and row-format
retrieval omits the key. A vector field currently accepts only a vector/array
shape when present; omit it rather than sending `null`.

An empty array on a multi-valued field likewise supplies no values. Use a
scalar for a single-valued field and an array for a field declared `multi`.
Supplying several values to a single-valued field is an update error.

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

Indexed terms, including IDs, are limited to 255 UTF-8-safe bytes. Longer IDs
are truncated in the term space, so values that share the same first 255-byte
prefix collide for overwrite and delete purposes. Keep IDs within that bound.

With the normal `allow_dups: false`, another document with the same ID replaces
the old document. Replacement is whole-document replacement: fields omitted by
the new version disappear. Solux does not currently implement field patches.
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
  they are not generic numeric multi-value fields.

Coercion is deliberately not a best-effort parser: trailing garbage, an object
where a scalar is expected, a wrong vector dimension, or an invalid coordinate
is a per-document error.

## Multi-valued shapes

The outer JSON shape follows the schema:

```json
{
  "tags_ss": ["fiction","classic"],
  "years_is": [1965,1969],
  "locations": [[-74.0060,40.7128],[-0.1278,51.5074]]
}
```

String, text, numeric, and date multi-fields use an array of scalar values. A
multi-geo field uses an array of `[lon,lat]` points. Multi-vector documents use
the typed protobuf `Val.arr_vec` arm; HTTP document arrays are not promoted to
vectors yet. The field must be declared `multi: true`; the `_ss`, `_is`, `_fs`,
`_ds`, `_dts`, and `_vs` default suffixes already are.

A multi-valued text field analyzes each input value separately and inserts a
position gap of `100` between values. A phrase therefore does not cross values
unless its slop reaches that gap.

## What is returned

Columns and stored fields solve different problems:

- A column retains a typed per-document value for sorting, faceting, numeric
  and geo queries, vector search, and retrieval where that type is supported.
- A stored field retains the original document value in compressed chunks.
  Text defaults to stored so retrieval returns the pre-analysis string.

Request `fields` explicitly. HTTP row format omits missing fields; HTTP column
format includes the requested key with `null`. Geo columns are a current
exception: they participate in queries but are not yet decoded by document
projection. See [Geo search](geo-search.md#current-limits).

Binary is reserved in the wire/schema enum but is not a usable engine field
type in the current release.
