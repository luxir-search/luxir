# Documents and values

A Luxir document is a flat JSON object with field names and values. The
schema defines each field's type and how it is indexed and stored:

```json
{
  "id": "b1",
  "title_t": "The Way of Kings",
  "year_i": 2010,
  "tags_ss": ["fantasy","epic"]
}
```

This page covers field templates, variants, IDs, and the values you can
send and retrieve.

## Field templates: types from field names

Field templates apply a type and other settings to fields with a matching
name suffix. Luxir's built-in templates let you start indexing without
defining your own schema:

- `title_t` is analyzed text;
- `category_s` is an indexed exact string;
- `year_i` is an integer column;
- `tags_ss` is a multi-valued exact string;
- `published_dt` is a date column;
- `embedding_v` is a vector column.

Templates also handle fields you cannot enumerate ahead of time. An ecommerce
catalog might define an `_attr` template for product attributes. New product
classes can then introduce fields such as `shoe_width_attr` or `socket_attr`
without a schema change for each new name. Define the rule once; new matching
fields work automatically. See the
[product attributes example](schema.md#field-templates-for-dynamic-fields).

You can combine templates with explicit field definitions, choosing which
fields to define individually and which to cover with a naming rule. Explicit
definitions take precedence over templates. The
[schema guide](schema.md#read-the-schema) lists the built-in suffixes and
explains how to customize fields and templates.

## Field variants

A field can index the same input in several ways. For example, you might want
to search an author's name by individual words, but facet and sort by the
whole name. [Field variants](schema.md#field-variants) let one field do both.

The built-in `_name` template already sets this up. Index these books with an
`author_name` field:

```http
POST /collections/authors/_update

{
  "docs": [
    {
      "id": "b1",
      "title_t": "Gridlinked",
      "author_name": "Neal Asher"
    },
    {
      "id": "b2",
      "title_t": "The Skinner",
      "author_name": "Neal Asher"
    },
    {
      "id": "b3",
      "title_t": "Snow Crash",
      "author_name": "Neal Stephenson"
    },
    {
      "id": "b4",
      "title_t": "Prador Moon",
      "author_name": "Neal Asher"
    },
    {
      "id": "b5",
      "title_t": "Anathem",
      "author_name": "Neal Stephenson"
    },
    {
      "id": "b6",
      "title_t": "Scythe",
      "author_name": "Neal Shusterman"
    }
  ],
  "commit": {}
}
```

Search for `neal`, return the first three matches by ID, and facet on the
same author field:

```http
POST /collections/authors/_search

{
  "query": "author_name:neal",
  "fields": ["title_t", "author_name"],
  "sort": "id",
  "limit": 3,
  "get_number": true,
  "ops": {
    "authors": {
      "field_facet": {
        "field": "author_name"
      }
    }
  }
}
```

```json
{
  "found": 6,
  "docs": [
    {
      "title_t": "Gridlinked",
      "author_name": "Neal Asher"
    },
    {
      "title_t": "The Skinner",
      "author_name": "Neal Asher"
    },
    {
      "title_t": "Snow Crash",
      "author_name": "Neal Stephenson"
    }
  ],
  "ops": {
    "authors": {
      "buckets": [
        {
          "val": "Neal Asher",
          "count": 3
        },
        {
          "val": "Neal Stephenson",
          "count": 2
        },
        {
          "val": "Neal Shusterman",
          "count": 1
        }
      ]
    }
  }
}
```

The word `neal` matches all six books. The facet groups them by the full
author name: three by Neal Asher, two by Neal Stephenson, and one by Neal
Shusterman. Facet counts include every match, so Shusterman appears in the
facet even though his book is beyond the first three results.

The template creates an analyzed text field for word search and a string
variant, `author_name__s`, for whole-name facets and sorting. Both operations
use `author_name` in the request; the template chooses the appropriate
representation. You only supply the name once when indexing.

Use `_names` for a multi-valued field, such as `author_names` for books with
several authors. You can also [define your own variants](schema.md#field-variants)
on explicitly named fields.

## IDs and replacement

`id` is the reserved unique-ID field. String and numeric values are accepted;
numeric IDs use their canonical string rendering, so `123` and `"123"` are
the same ID.

With the default `allow_dups: false`, another document with the same ID
replaces the old document. Replacement is whole-document replacement: fields
omitted by the new version disappear.  Set `allow_dups: true` only when
you know you are sending unique documents.

An ID is not required. A document with an absent or null ID can be searched,
but it cannot be overwritten or deleted by ID and has no useful external
identity.  `_version_` is reserved for internal overwrite ordering and is not
an application field.

## Multi-valued fields

The outer JSON shape follows the schema:

```json
{
  "tags_ss": ["fiction","classic"],
  "years_is": [1965,1969],
  "locations": [[-74.0060,40.7128],[-0.1278,51.5074]],
  "embedding_vs": [[0.8,0.1,0.1],[0.2,0.7,0.1]]
}
```

String, text, numeric, and date multi-fields use an array of scalar values.
A multi-geo field uses an array of `[lon,lat]` points. A multi-vector field
uses an array of number arrays; a bare number array is also accepted as a
one-vector list. The field must be declared `multi: true`; the `_ss`, `_is`,
`_fs`, `_ds`, `_dts`, and `_vs` default suffixes already are.

## What is returned

Columns and stored fields solve different problems:

- A column retains a typed per-document value for sorting, faceting, numeric
  and geo queries, vector search, and retrieval where that type is supported.
- Stored text, string, and ID fields keep source text before analysis or
  normalization. Text fields are stored by default, so searching individual
  words still lets you retrieve the original title or author name.

Use `fields` to choose which values to return, as in the example above. You
can also use `*` wildcard patterns such as `"attr_*"`. Omitting `fields`
returns every retrievable logical field except vectors and engine fields.
See [field retrieval](searching.md#field-retrieval-and-result-shape) for the
full projection rules.

## Details

### Scalar coercion

Index-time and query-time coercion use the same rules: a literal accepted for
a field at ingest can be used to match or bound that field later.

- String, text, and ID fields accept strings. Numeric and boolean scalars are
  converted to their canonical text rendering.
- Integer fields accept JSON integers, integral floating-point numbers, and
  strings that parse to an integer within the signed 64-bit range.
- Float and double fields accept JSON numbers and numeric strings.
- Date fields accept epoch milliseconds, ISO-8601 strings, partial dates, and
  date math. Update messages interpret offset-less date text in UTC; search
  requests may supply a query time zone. See [Dates](dates.md).
- Geo points and vectors have array shapes described in their dedicated
  guides; they are not generic numeric multi-value fields. A vector is a
  number array; integer elements are accepted, while doubles must narrow to
  finite float32.

Numeric primaries return their typed column; `stored` is ignored for them.
Each variant converts the submitted value independently. For example, an
integer field `edition` with a string variant `label` converts `"0042"` to
numeric `42`, while `edition__label` keeps `"0042"`. Reindexing needs the
producer's input, not just the primary's retrieved value.

### Stored values and variant retrieval

Stored text, string, and ID fields retain canonical source text. Numeric and
boolean inputs become text: a numeric `42` stored as TEXT returns `"42"`.

Bare retrieval and `__self` use the primary's stored source when enabled,
otherwise its own column. A normalized STRING primary without storage returns
its normalized column value. Variants have no stored copy: an exact selector
returns that representation's column under the selector key, and a TEXT
variant has no retrievable value.

Patterns without `__` never expand variants; `author__*` or `*__s` discovers
variants that have a column. HTTP row format omits missing fields; HTTP
column format includes the requested key with `null`.

For a multi-valued field, stored values keep source order and duplicates;
a string column is a sorted, deduplicated set.

### Multi-valued text and phrases

A multi-valued text field analyzes each input value separately and inserts a
position gap of `100` between values, so a phrase does not cross values
unless its slop reaches that gap.

## Limits

- Nested objects and nested documents are not a field type. Model
  relationships with IDs or flatten the fields in the producer.
- Geo columns participate in queries but are not yet decoded by document
  projection; see [Geo search](geo-search.md#current-limits).
- Binary is reserved in the wire/schema enum but is not a usable engine field
  type in the current release.
