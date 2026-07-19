# Schema

Solux works without a schema: field types come from name suffixes (`title_w`,
`year_i`, `tags_ss` - see the [Quickstart](quickstart.md)). When you want real
field names without suffixes, a custom analyzer, or typed vector fields, you
define a schema. The schema API speaks the same JSON in both directions: what
`GET` returns is exactly what you `POST`.

## Read the schema

```bash
curl http://localhost:9400/collections/main/_schema
```

```json
{
  "fields": {
    "id": {
      "type": "id"
    },
    "_version_": {
      "type": "int"
    }
  },
  "templates": {
    "_i": {
      "type": "int"
    },
    "_s": {
      "type": "string"
    },
    "_t": {
      "type": "text",
      "analyzer": {
        "tokenizer": "unicode_word",
        "filters": ["nfkc_cf", "fold"]
      }
    },
    "_w": {
      "type": "text",
      "analyzer": {
        "tokenizer": "whitespace"
      }
    }
  }
}
```

(Abbreviated - the default schema defines templates for every suffix.)

The complete suffix set is:

| Suffix | Type and default behavior |
|---|---|
| `_s` | Indexed exact string with a column. |
| `_sc` | Column-only exact string. |
| `_ss`, `_ssc` | Multi-valued forms of `_s` and `_sc`. |
| `_i`, `_is` | Integer column, single- or multi-valued. |
| `_f`, `_fs` | Float column, single- or multi-valued. |
| `_d`, `_ds` | Double column, single- or multi-valued. |
| `_dt`, `_dts` | Date column, single- or multi-valued. |
| `_w` | Stored text split on whitespace, case- and accent-sensitive. |
| `_wl` | Stored Unicode-word text with NFKC case folding; accents preserved. |
| `_t` | Stored Unicode-word text with NFKC case and accent folding. |
| `_v`, `_vs` | Single- or multi-valued vector column; storage-only until a metric is set on a concrete field. |

Numeric suffixes are column-backed but do not build a points index by default;
range and exact-match queries still work by scanning the column. Define a
concrete field with `index: "range"` when those operations need a points index.
There is no default geo suffix because coordinate fields benefit from an
unambiguous explicit definition.

Two sections:

- **`fields`** are concrete: a document field named `title` uses
  `fields.title`, exactly.
- **`templates`** are the suffix rules. A field named `anything_t` that has no
  exact entry in `fields` picks up the `_t` template. Templates are never
  usable as document fields themselves, and any definition can name one as
  `parent` to inherit its properties.

## Define fields

Writes are `POST`; the operation is the `mode` query parameter, visible right
in the URL. The default, `mode=set`, sets each named definition and leaves
everything else alone:

```bash
curl http://localhost:9400/collections/main/_schema -d '{
  "fields": {
    "title": {"type": "text", "stored": true,
              "analyzer": {"tokenizer": "unicode_word", "filters": ["nfkc_cf", "fold"]}},
    "year":  {"type": "int", "index": "range"},
    "vec":   {"type": "vector", "dims": 768, "metric": "cosine"}
  }
}'
```

The response is the full resulting schema - the same shape `GET` returns.

`set` works at whole-definition granularity: setting a name that already
exists replaces that field's entire definition with what you sent (it never
merges individual properties into the old one). Setting `title` to
`{"stored": false}` doesn't keep the old type and analyzer - it defines a
field with only `stored`, which fails with a "no type" error.

When a definition only needs a type, a bare string works:

```bash
curl http://localhost:9400/collections/main/_schema \
  -d '{"fields": {"year": "int", "author": "string"}}'
```

`mode=replace_all` replaces the whole schema with exactly what you send:

```bash
curl 'http://localhost:9400/collections/main/_schema?mode=replace_all' -d @schema.json
```

The reserved `id` and `_version_` fields are materialized automatically if you
omit them, so a minimal `replace_all` cannot break indexing - but one that
doesn't re-list the suffix templates removes them (strict-schema mode, in
effect). Start from `GET` output if you want to edit rather than replace:
posting a `GET` body back is a no-op under either mode.

## Field properties

| Key | Meaning |
|---|---|
| `type` | `string`, `text`, `int`, `float`, `double`, `date`, `vector`, `geo_point`, `id` |
| `index` | `match`, `range`, or `none`; absent = the type's default (`text`/`string` index for match, numerics don't) |
| `column` | store values in a per-field column (sorting, faceting, analytics); supported and default-on for non-`text` types; `column:true` is rejected for analyzed text |
| `multi` | multi-valued |
| `stored` | keep raw values for retrieval; default on for `text` only |
| `stored_resource` | stored-field column family; empty uses the default `_stored_` resource |
| `analyzer` | `text` only: `{"tokenizer": ..., "filters": [...]}`; tokenizers: `whitespace`, `keyword`, `unicode_word`; filters: `lowercase`, `nfkc_cf`, `fold` |
| `parent` | inherit any unset properties from a field or template |
| `dims`, `metric`, `normalized`, `normalize_on_write` | `vector` only; `metric`: `l2`, `ip`, `cosine`, `none` |

Every property is optional. Absent means "inherit from `parent`, else the
type's default" - and the schema you read back stays as sparse as the one you
wrote.

`column` and `stored` solve different problems. A column is a typed,
per-field structure used by sorting, faceting, analytics, numeric/geo queries,
and vector search. Stored fields preserve document values for retrieval in
compressed chunks. Analyzed text has postings and stored retrieval but no
per-document value column; use a parallel `string` field when the same source
value must also sort or facet. Scalar types generally return their values from
columns without a second stored copy.

Mistakes are errors, not surprises: an unknown property, type, tokenizer, or
filter name gets a `400` naming the valid choices; redefining `id` as anything
but an id field is rejected.

## The operations

| Request | Meaning |
|---|---|
| `GET /collections/{c}/_schema` | the authored schema (valid write body) |
| `POST /collections/{c}/_schema` | `mode=set` (default): set the named definitions, keep the rest |
| `POST /collections/{c}/_schema?mode=replace_all` | replace the whole schema |

Schema changes apply to newly indexed documents; already-indexed segments are
not rewritten. Changing a field's type under existing data is not checked yet -
prefer additive changes on live collections.
