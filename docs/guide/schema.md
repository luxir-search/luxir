# Schema

Luxir works without a schema: field types come from name suffixes (`title_t`,
`year_i`, `tags_ss` - see the [Quickstart](quickstart.md)). When you want real
field names without suffixes, a custom analyzer, or typed vector fields, you
define a schema. The schema API speaks the same JSON in both directions: what
`GET` returns is a valid `POST` body, and posting it back keeps the same
definitions (each successful publication advances the schema generation).

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
| `_wl` | Stored text split on whitespace, Unicode-lowercased; no normalization or accent folding. |
| `_u` | Stored Unicode-word text, case- and accent-sensitive. |
| `_un` | Stored Unicode-word text with NFKC case folding; accents preserved. |
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

Analyzer components work the same way. Each tokenizer or filter is
`{"name": ..., "params": {...}}`; a component with no parameters can be
written as its bare name, which is what the `title` example above does, and
reads back the same way. Parameters are typed JSON values (a string, number,
bool, or list), and a component that takes none rejects any.

`mode=replace_all` replaces the whole schema with exactly what you send:

```bash
curl 'http://localhost:9400/collections/main/_schema?mode=replace_all' -d @schema.json
```

The reserved `id` and `_version_` fields are materialized automatically if you
omit them, so a minimal `replace_all` cannot break indexing - but one that
doesn't re-list the suffix templates removes them (strict-schema mode, in
effect). Start from `GET` output if you want to edit rather than replace:
posting an authored `GET` body back keeps the same definitions under either mode.

## Field properties

| Key | Meaning |
|---|---|
| `type` | `string`, `text`, `int`, `float`, `double`, `date`, `vector`, `geo_point`, `id` |
| `index` | `match`, `range`, or `none`; absent = the type's default (`text`/`string` index for match, numerics don't) |
| `column` | store values in a per-field column (sorting, faceting, analytics); supported and default-on for non-`text` types; `column:true` is rejected for analyzed text |
| `multi` | multi-valued |
| `stored` | keep canonical source text for TEXT/STRING/ID retrieval, before analysis/normalization; default on for `text` only; ignored for numerics |
| `stored_resource` | stored-field column family; empty uses the default `_stored_` resource |
| `analyzer` | `text` only: `{"tokenizer": <component>, "filters": [<component>, ...]}`, a component being `{"name": ..., "params": {...}}` or a bare name; tokenizers: `whitespace` (default), `keyword`, `unicode_word`; filters: `lowercase`, `nfkc_cf`, `fold` (none take parameters yet) |
| `long_terms` | `string` and `text`: `truncate` (default) or `reject` for normalized whole strings or analyzed tokens over 255 bytes; invalid on `string` with `index: "none"`. Use `reject` when accepting a shared prefix as the indexed value would be incorrect. |
| `normalizer` | `string` only: a list of filter components applied to each whole value, with no tokenizer. |
| `variants` | Map from label to another field definition receiving the same input value. Bare type strings work here too. |
| `defaults` | `search` and `value` bindings, each naming `self` or a variant label; both default to `self`. |
| `parent` | inherit any unset properties from a field or template |
| `dims`, `metric`, `normalized`, `normalize_on_write` | `vector` only; `metric`: `l2`, `ip`, `cosine`, `none` |

Every property is optional. Absent means "inherit from `parent`, else the
type's default" - and the schema you read back stays as sparse as the one you
wrote.

`analyzer` inherits as one unit: an empty `{}` inherits the parent's analyzer,
while naming a tokenizer or listing any filter replaces the whole chain (filters
alone get the `whitespace` tokenizer). A tokenizer component you do write must
have a name.

`column` and `stored` solve different problems. A column is a typed,
per-field structure used by sorting, faceting, analytics, numeric/geo queries,
and vector search. Stored fields keep canonical source text for retrieval in
compressed chunks. Analyzed text has postings and stored retrieval but no
per-document value column; use a `string` variant when the same source
value must also sort or facet. Scalar types generally return their values from
columns without a second stored copy.

Mistakes are errors, not surprises: an unknown property, type, tokenizer, or
filter name gets a `400` naming the valid choices; redefining `id` as anything
but an id field is rejected.

## Field variants

A document supplies one value per logical field. The primary keeps the field's
name; each variant indexes the same submitted value under `<field>__<label>`.
For example, create a collection with text search and whole-author values:

```http
POST /collections/_create

{"name":"names"}
```

```http
POST /collections/names/_schema

{
  "fields": {
    "author": {
      "parent": "_t",
      "variants": {"s": {"type":"string","normalizer":["nfkc_cf","fold"]}},
      "defaults": {"value":"s"}
    },
    "edition": {"type":"int","index":"range","variants":{"label":"string"}},
    "genre": {"type":"string","variants":{"t":{"parent":"_t"}},"defaults":{"search":"t"}}
  }
}
```

```http
POST /collections/names/_update

{
  "docs": [
    {"id":"b1","author":"Ursula K. Le Guin","edition":"0042","genre":"Science Fiction"},
    {"id":"b2","author":"George R.R. Martin","edition":10,"genre":"Fantasy"},
    {"id":"b3","author":"LE GUIN","edition":2,"genre":"Science Fiction"}
  ],
  "commit": {}
}
```

`author` searches analyzed text; exact membership, facets, ranges, and sorts
use `author__s`. Retrieval of `author` returns the source spelling.
`genre` searches through `genre__t` and uses its primary for value operations.
`edition` coerces `"0042"` to numeric `42`, while `edition__label` keeps `"0042"`.
Every branch converts the submitted value itself.

### Names and shape

Labels are ASCII: a letter first, then letters, digits, or single underscores.
`self` is reserved case-insensitively, and labels differing only by case
collide. Label lookup otherwise uses the authored case. `__` is reserved in
authored field names, template names, labels, and final document keys after
field mapping. It is only used in requests to select a representation.
`author__self` selects the primary; it does not create another physical field.

Physical names are limited to 127 bytes. Concrete definitions that exceed the
limit are schema errors. A template's maximum instance name length is
`127 - 2 - longest label length`; an overlong instance fails the document or
request that first resolves it. The `__self` selector is not a physical name
and does not consume that budget.

`multi`, `stored`, and `stored_resource` belong to the logical field. A variant
may not set any of them, even to `false` or an empty string. It also may not
declare nested `variants` or `defaults`. There is one stored source on the
primary when storage is enabled. Neither `id` nor `_version_` may have variants,
including inherited ones, and a variant cannot have type `id`.

The primary and every variant must belong to the same shape family:

| Family | Allowed types |
|---|---|
| Scalar | `text`, `string`, `int`, `float`, `double`, `date`. |
| Vector | `vector`; every branch must resolve to the same positive `dims`. Inferred dimensions (`0`) are not allowed with variants. |
| Geo | `geo_point`. |

### Default bindings

Each binding defaults to `self`; adding a variant does not select it
automatically. An explicit selector such as `author__s` or `author__self`
bypasses both bindings.

| Operation | Bare field name uses |
|---|---|
| Match, phrase, simple query, expression terms/phrases, prefix, fuzzy, wildcard, regex | `defaults.search`, including matches inside filters. |
| `any_of`, `:=`, ranges, field/range facets, sort, column expressions, metrics | `defaults.value`, then the operation's capability checks. |
| Retrieval, exists, kNN, geo | The primary; retrieval uses its source store or its own typed column. |

A binding names a local label, not another field. A label missing from the
effective variant map is a schema error. See [Searching](searching.md#field-bindings)
for query and projection examples.

### Templates and inheritance

Templates can carry variants and defaults. A template for names, with a
multi-valued form inheriting the same representations:

```http
POST /collections/names/_schema

{
  "templates": {
    "_name": {
      "parent": "_t",
      "variants": {"s":{"parent":"_s","normalizer":["nfkc_cf","fold"]}},
      "defaults": {"value":"s"}
    },
    "_names": {"parent":"_name","multi":true}
  }
}
```

`author_name` uses `_name`; `author_name__s` selects its normalized string
variant. The root is resolved before the label, so the tail `_s` never picks
the default string template independently. These templates are opt-in;
long whole names follow the STRING truncation policy below.

`_name` inherits `_t` for word search; a bare `type: text` would use the
whitespace analyzer.

`variants` and `defaults` each inherit atomically: absent inherits, present
replaces the whole object, and `{}` clears it. There is no per-label merge.
For example, after defining `_name`:

```http
POST /collections/names/_schema

{
  "fields": {
    "inherited": {"parent":"_name"},
    "replaced": {"parent":"_name","variants":{"raw":"string"},"defaults":{"value":"raw"}},
    "cleared": {"parent":"_name","variants":{},"defaults":{}}
  }
}
```

`inherited` keeps `s` and the value binding. `replaced` has only `raw`.
`cleared` has only its primary and both bindings are `self`. Clearing variants
without clearing an inherited binding to `s` fails with a dangling-label error.
Within a present `defaults`, an omitted binding becomes `self`.

A variant's own `parent` borrows physical settings only: type, index, column,
analyzer, normalizer, `long_terms`, and vector settings. It does not inherit that parent's
shape, storage, variants, or defaults. Parents name authored fields or templates,
never derived selectors.

A new suffix can inherit `_t`'s word analysis and add a whole-value variant:

```http
POST /collections/_create

{
  "name": "templates",
  "schema": {"templates": {
    "_title": {"parent":"_t","variants":{"s":"string"},"defaults":{"value":"s"}}
  }}
}
```

```http
POST /collections/templates/_update

{"docs":[{"id":"t1","book_title":"Dune"}],"commit":{}}
```

This indexes `book_title` and `book_title__s`. `_title` inherits `_t`'s
case- and accent-folding analysis. Long titles keep their full stored source;
the string variant indexes a truncated prefix for sorting, faceting, and exact
lookup.

### STRING normalization and length

`normalizer` takes filters only: `lowercase`, `nfkc_cf`, and `fold`, using the
same component syntax as analyzer filters. Each input element stays one whole
value. Normalization applies at ingest and to query literals, facet `selected`
values, and range bounds. Absent `normalizer` inherits; `[]` clears it.

Indexed STRING values truncate to at most 255 bytes after normalization,
backing off to a UTF-8 boundary. TEXT tokens truncate the same way after
analysis. This applies to each element or token, not the total document length.
Exact literals, `any_of` values, range bounds, and facet `selected` values use
the same policy; TEXT exact membership applies it to the single analyzed term.
Patterns keep their own rules. See [term-space limits](documents.md#ids-and-replacement)
for prefix collisions and the stored-source contract.

Set `long_terms: "reject"` to fail a document containing an over-limit string
or token and to report a teaching error for an over-limit exact lookup or bound.
Absent `long_terms` inherits through the parent chain, otherwise defaults to
`truncate`; explicitly setting `truncate` overrides inherited `reject`. Variants
may set the policy or borrow it through their own `parent`; they do not inherit
it from their logical primary. The setting, including an inherited setting, is
invalid on other types or on a STRING with `index: "none"`. Column-only strings
have no term-space limit. IDs always truncate.

The policy can change on a live field without reindexing. Existing terms and
stored values keep their meaning; changing to `reject` does not undo earlier
truncation or separate colliding prefixes.

## The operations

| Request | Meaning |
|---|---|
| `GET /collections/{c}/_schema` | the authored schema (valid write body) |
| `GET /collections/{c}/_schema?view=resolved` | effective representations, bindings, and schema coverage (HTTP only) |
| `POST /collections/{c}/_schema` | `mode=set` (default): set the named definitions, keep the rest |
| `POST /collections/{c}/_schema?mode=replace_all` | replace the whole schema |

### Resolved view

```http
GET /collections/names/_schema?view=resolved
```

The default GET, also available as `?view=authored`, stays sparse: explicit
parents, variants, and defaults are returned without inheritance expansion.
The resolved view is diagnostic, not a schema write body. It contains:

| Property | Meaning |
|---|---|
| `generation` | Current schema generation; the initial default schema is generation `0`. |
| `fields` | Logical concrete fields, including materialized suffix-template instances. |
| `fields.<f>.bindings` | `search` and `value`, each naming the effective physical field. |
| `fields.<f>.representations` | Definitions keyed by `self` or variant label. |
| Representation settings | `name`, `type`, `multi`, `index`, `column`, `stored`, `analyzer`, `normalizer`, plus `stored_resource` and vector settings when applicable. These describe the structures available to operations. |
| `introduced_generation` | Start of this representation's current continuous availability. A compatible removed/re-added label gets a new introduction. |
| `oldest_generation` | Minimum schema generation across all committed live segments, even those lacking this field; `null` for an empty index. |
| `coverage_complete` | True for an empty index or when that minimum reaches the introduction generation. |

Coverage is conservative schema provenance, not a count of documents containing
the field. Merges keep the minimum input generation and cannot establish
backfill. Deleting some documents from an old segment does not refine its
generation.

### Changes on a live collection

Schema changes do not rewrite existing documents. Adding `author__s` leaves
older documents without it; switching `defaults.value` to `s` immediately
routes new requests there. Those older documents will not match its exact
queries or contribute to its value facets. Reindex from the producer's input
to populate it; retrieval is not guaranteed to reproduce every branch's input.

Each admitted update message pins one schema generation. Messages admitted
after a successful publication use the new schema; earlier messages finish
with their original one. An incompatible definition edit may briefly block
new admission while already admitted work drains, then reject if that work
actually used the old definition.

Once a physical name has materialized data, its indexed meaning cannot change:
type, `multi`, index/column settings, posting settings, analyzer or normalizer,
and vector parameters must remain compatible. A rejected schema edit returns
`400` with `code: "invalid_schema"`, names the physical field and changed
property, and leaves the current schema in place. The rule includes template
edits affecting materialized dynamic roots. Unused definitions can be corrected.

Removing a variant does not free its name. Successfully flushed names remain
reserved until the index is cleared, even if their old segments disappear.
Use a new field or variant label and reindex to change the representation.
Stored-source settings and default bindings can change without changing the
indexed meaning, but neither change fills missing old values.
