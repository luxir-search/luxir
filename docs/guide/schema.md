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
| `long_terms` | `string`, `text` (per token), and `id`: `hash128` (default), `truncate`, or `reject` for terms over 255 bytes after normalization/analysis. Inherits from `parent`; invalid on column-only strings and other types. Changes do not rewrite existing terms. |
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
      "variants": {"s": {"type":"string","normalizer":["nfkc_cf","fold"],"long_terms":"hash128"}},
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
long whole names follow the STRING term policy below.

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
the string variant indexes a prefix plus hash suffix for sorting, faceting, and
exact lookup.

### STRING normalization and length

`normalizer` takes filters only: `lowercase`, `nfkc_cf`, and `fold`, using the
same component syntax as analyzer filters. Each input element stays one whole
value. Normalization applies at ingest and to query literals, facet `selected`
values, and range bounds. Absent `normalizer` inherits; `[]` clears it.

Indexed STRING values after normalization, TEXT tokens after analysis, and IDs
share a 255-byte term space. Terms of 255 bytes or less stay unchanged. The
default `long_terms` is `hash128`: longer terms become the first 230 bytes,
backed off to a UTF-8 boundary, followed immediately by 25 base36 digits. There
is no delimiter. The suffix is unseeded XXH3_128 of the whole term, taken as
one 128-bit number from its canonical 16 bytes (high 64 bits then low 64 bits,
both big-endian) and written in `0-9` and `a-z`, most significant digit first,
zero padded to 25 digits. These format choices are fixed for `hash128`.

Different long values retain distinct exact matches and facet buckets except
for hash collisions. This is not attack-resistant: xxHash is not cryptographic,
and an ordinary short term can also equal a generated term. Sorts and ranges
agree with source byte order up to the kept prefix; beyond it they compare the
hash suffix, not the source tail. Term enumeration, facets, and indexed columns
return the term as stored. Stored source keeps full bytes before normalization
or hashing. Column-only strings have no term-space limit.

Exact literals, `any_of` / `:=`, range bounds, and facet `selected` values use
the same policy; TEXT exact membership applies it to the single analyzed term.
Match and phrase apply it per analyzed token. Prefix queries longer than the
kept prefix fall back to that prefix, so they return a superset. Wildcard and
regex queries do the same when their common leading literal prefix exceeds
the limit; other patterns operate on stored term bytes. Fuzzy distance also
compares transformed term bytes. See [term-space limits](documents.md#ids-and-replacement)
for the implications of submitting returned hash terms to normalization.

`long_terms: "truncate"` cuts at a UTF-8 boundary at or below 255 bytes
instead, merging values with the same retained prefix (including IDs for
overwrite/delete). `long_terms: "reject"` fails the document for an over-limit
term and reports a teaching error for an over-limit query term, bound, or
selection. A rejecting variant fails its entire document. Invalid delete IDs
are request errors.

Absent `long_terms` inherits through the parent chain, otherwise defaults to
`hash128`; an explicit policy overrides inheritance. Variants may set the policy
or borrow it through their own `parent`; they do not inherit it from their
logical primary. An explicitly set or inherited policy is invalid on other
types or on a STRING with `index: "none"`.

Changing the effective policy affects newly admitted updates and new queries;
existing segments keep their original terms. Use a new field or variant label
and reindex to change the policy safely.

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
| `fields` | Declared concrete fields and their variants; suffix-template instances are not listed. |
| `fields.<f>.bindings` | `search` and `value`, each naming the effective physical field. |
| `fields.<f>.representations` | Definitions keyed by `self` or variant label. |
| Representation settings | `name`, `type`, `multi`, `index`, `column`, `stored`, `analyzer`, `normalizer`, effective `long_terms` for term-backed representations, plus `stored_resource` and vector settings when applicable. These describe the structures available to operations. |
| `introduced_generation` | Start of this representation's current continuous availability. Redefining a representation or removing and re-adding its label starts a new introduction. |
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

Each admitted update message pins one schema generation. A schema change takes
effect for updates admitted after the schema call returns; earlier messages
finish with their original one. The call does not wait for those messages.
Each inverter keeps its schema for its whole life. A stale idle inverter
flushes at the next checkout, and a stale busy one flushes when released, so
each segment is written under a single schema.

Schema changes are not validated against existing data. Existing segments keep
the representation they were written with, even when a field or template is
redefined. This applies to type, `multi`, index/column settings, posting
settings, analyzer or normalizer, `long_terms`, and vector parameters. Accepting
a schema edit does not establish that new queries can use the old data correctly.

Removing a variant does not remove its data from existing segments. Reusing
its label with a different definition can leave old and new representations
under the same physical name. Use a new field or variant label and reindex to
change a representation safely. Stored-source settings and default bindings
can also change, but neither change fills missing old values.
