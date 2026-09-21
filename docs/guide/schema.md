# Schema

The schema defines field types, text analysis, storage, and indexes. You can
define fields individually, use field templates for names that share a suffix,
or combine both. Built-in templates let you start without writing your own
schema. Templates also handle new fields as your data grows.

## Field types

Choose a type based on how you want to search and use the values:

| Type | Description and typical uses |
|---|---|
| `text` | Analyzed text for full-text search: titles, descriptions, and article bodies. An analyzer splits each value into searchable tokens for word and phrase queries. |
| `string` | A whole value for exact matching, filtering, sorting, and faceting: categories, tags, and product codes. Values are not split into words. |
| `int` | Signed 64-bit integer: counts, quantities, and years. |
| `float` | 32-bit floating-point number: measurements and other numeric values where single precision is sufficient. |
| `double` | 64-bit floating-point number for greater precision than `float`. |
| `date` | Timestamp with millisecond precision, supplied as ISO-8601 text or epoch milliseconds. Supports date queries, ranges, and calendar facets. See [Dates](dates.md). |
| `vector` | Dense vector of floating-point values, such as an embedding. Set a similarity metric to enable nearest-neighbor search. See [Vector search](vector-search.md). |
| `geo_point` | Geographic location supplied as `[longitude, latitude]`, for distance and bounding-box queries. See [Geo search](geo-search.md). |
| `id` | Unique document identifier, reserved for the built-in `id` field. Used to identify documents for replacement and deletion. |

### `text` versus `string`

Both accept text values, but they make different things searchable. Suppose
you index `"Red Bicycle"` using the built-in templates:

- **`title_t` (`text`)** indexes the words `red` and `bicycle`.
  `title_t:bicycle` matches, as does `title_t:"red bicycle"`.
- **`title_s` (`string`)** indexes the whole value `Red Bicycle`.
  `title_s:"Red Bicycle"` matches; `title_s:bicycle` does not.

Sorting text values requires a whole-value column, which `string` provides.
Faceting can use analyzed terms from a `text` field, but whole values are
often more appropriate, such as complete author names or categories.

The analyzer controls how `text` is split and normalized. Bare `"type": "text"`
defaults to splitting on whitespace with no filters; the `_t` template adds
Unicode word segmentation, case and accent folding, and English stemming.
See [Text analysis](#text-analysis) for the available components.

`string` keeps values verbatim by default. You can configure a
[normalizer](#string-normalization-and-length) to transform them, for example
by folding case or accents, without splitting them into words.
When one value needs both full-text search and whole-value
sorting or faceting, use a `text` field with a `string`
[variant](#field-variants). The built-in `_name` template provides this
combination for names.

## Read the schema

The schema API uses the same JSON in both directions: what `GET` returns is
a valid `POST` body, and posting it back keeps the same definitions.

```http
GET /collections/main/_schema
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
        "filters": ["nfkc_cf", "fold", "kstem"]
      }
    },
    "_name": {
      "type": "text",
      "analyzer": {
        "tokenizer": "unicode_word",
        "filters": ["nfkc_cf", "fold"]
      },
      "variants": {
        "s": {
          "parent": "_s"
        }
      },
      "defaults": {
        "value": "s"
      }
    }
  }
}
```

(Abbreviated: the default schema defines a template for every suffix.)

Two sections:

- **`fields`** define individual fields. A document field named `title` uses
  `fields.title`. Explicit definitions take precedence over templates.
- **`templates`** define shared settings for fields whose names end in a
  matching suffix. A field named `anything_t` with no explicit definition uses
  the `_t` template. Matching uses the suffix from the final underscore.
  Templates are never usable as document fields themselves, and any definition
  can name one as `parent` to inherit its properties.

The built-in templates are:

| Suffix | Type and default behavior |
|---|---|
| `_s` | Indexed exact string with a column. |
| `_sc` | Column-only exact string. |
| `_ss`, `_ssc` | Multi-valued forms of `_s` and `_sc`. |
| `_i`, `_is` | Integer column, single- or multi-valued. |
| `_f`, `_fs` | Float column, single- or multi-valued. |
| `_d`, `_ds` | Double column, single- or multi-valued. |
| `_dt`, `_dts` | Date column, single- or multi-valued. |
| `_t` | Unicode-word text with NFKC case folding, accent folding, English possessive removal, and KStem English stemming. The general-purpose text field. |
| `_u` | Unicode-word text, case- and accent-sensitive. |
| `_un` | Unicode-word text with NFKC case folding; accents preserved, no stemming. |
| `_w` | Text split on whitespace, case- and accent-sensitive. |
| `_wl` | Text split on whitespace, Unicode-lowercased; no normalization or accent folding. |
| `_name` | Unicode-word text with case and accent folding and no stemming, plus a whole-value string variant for facets and sorting. |
| `_names` | Multi-valued form of `_name`. |
| `_v`, `_vs` | Single- or multi-valued vector column; storage-only until a metric is set on a concrete field. |

Text fields are **stored** in the default compressed **stored_resource**.
Numeric suffixes are column-backed but do not build a points index by default;
range and exact-match queries still work by quickly scanning the column. Define a
concrete field with `index: "range"` when those operations need a points index.
There is no default geo suffix; define geo fields explicitly.

## Field templates for dynamic fields

Templates are useful when you do not know every field name ahead of time. An
ecommerce catalog can gain new product classes, each with its own attributes.
Define their shared settings once, and new matching fields work automatically.

This schema combines an explicit `productname` field with a custom `_attr` template
for string attributes. The built-in templates remain available:

```http
POST /collections/catalog/_schema

{
  "fields": {"productname": {"parent": "_t"}},
  "templates": {"_attr": "string"}
}
```

Index a product with a `shoe_width_attr` field:

```http
POST /collections/catalog/_update

{"docs": [{"id": "shoe1", "productname": "Trail shoes", "shoe_width_attr": "wide"}], "commit": {}}
```

Later, a new product class introduces `socket_attr`. No schema update is
needed:

```http
POST /collections/catalog/_update

{"docs": [{"id": "cpu1", "productname": "Desktop processor", "socket_attr": "AM5"}], "commit": {}}
```

Both attribute fields use `_attr`'s string settings, so you can filter, sort,
and facet on them immediately after the commit:

```http
POST /collections/catalog/_search

{"query": "socket_attr:AM5", "fields": ["id", "productname", "socket_attr"]}
```

```json
{
  "docs": [
    {
      "id": "cpu1",
      "productname": "Desktop processor",
      "socket_attr": "AM5"
    }
  ]
}
```

An explicit definition overrides a template for that field. You can also
define every field individually if the field names are known in advance.

## Define fields

Use `POST` to define fields. The default, `mode=set`, sets each named
definition and leaves everything else alone:

```http
POST /collections/main/_schema

{
  "fields": {
    "title": {
      "type": "text",
      "stored": true,
      "analyzer": {
        "tokenizer": "unicode_word",
        "filters": ["nfkc_cf", "fold"]
      }
    },
    "year": {
      "type": "int",
      "index": "range"
    },
    "vec": {
      "type": "vector",
      "dims": 768,
      "metric": "cosine"
    }
  }
}
```

The response is the full resulting schema, the same shape `GET` returns.

When a definition only needs a type, a bare string works:

```http
POST /collections/main/_schema

{"fields": {"year": "int", "author": "string"}}
```

Setting an existing field replaces its entire definition.

Each tokenizer or filter is `{"name": ..., "params": {...}}`. Components
without parameters can use a bare name, as in the `title` example above.
Parameters are typed JSON values (a string, number, bool, or list).

`mode=replace_all` replaces the whole schema:

```http
POST /collections/main/_schema?mode=replace_all

{"fields": {"title": "text", "year": "int"}}
```

The reserved `id` and `_version_` fields are added automatically if omitted.
Omitting templates removes them, leaving a schema with only explicit fields.
To edit the existing schema, start from `GET` output. Each successful
publication advances the schema generation.

## Field properties

| Key | Meaning |
|---|---|
| `type` | The [field type](#field-types): `text`, `string`, `int`, `float`, `double`, `date`, `vector`, `geo_point`, `id` |
| `index` | `match`, `range`, or `none`; absent = the type's default (`text`/`string` index for match, numerics don't) |
| `column` | store values in a per-field column (sorting, faceting, analytics); supported and default-on for non-`text` types |
| `multi` | multi-valued |
| `stored` | keep canonical source text for TEXT/STRING/ID retrieval, before analysis/normalization; default on for `text` only; ignored for numerics |
| `stored_resource` | stored-field group for TEXT/STRING/ID; empty or absent inherits, falling back to `_stored_` |
| `analyzer` | `text` only: `{"tokenizer": <component>, "filters": [<component>, ...]}`; see [Text analysis](#text-analysis) |
| `normalizer` | `string` only: a list of filter components applied to each whole value, with no tokenizer; see [STRING normalization and length](#string-normalization-and-length) |
| `long_terms` | `string`, `text` (per token), and `id`: `hash128` (default), `truncate`, or `reject` for terms over 255 bytes after normalization/analysis; see [Long terms](#long-terms) |
| `variants` | Map from label to another field definition receiving the same input value; bare type strings work here too; see [Field variants](#field-variants) |
| `defaults` | For fields with variants, choose `self` or a variant label to override the inferred `search` and `value` bindings; see [Default bindings for variants](#default-bindings-for-variants) |
| `parent` | inherit any unset properties from a field or template |
| `dims`, `metric`, `normalized`, `normalize_on_write` | `vector` only; `metric`: `l2`, `ip`, `cosine`, `none`; see [Vector search](vector-search.md) |

Every property is optional. Absent means "inherit from `parent`, else the
type's default", and the schema you read back stays as sparse as the one you
wrote.

`analyzer` inherits as one unit: an empty `{}` inherits the parent's analyzer,
while naming a tokenizer or listing any filter replaces the whole chain
(filters alone get the `whitespace` tokenizer).

`column` and `stored` solve different problems. A column is a typed,
per-field structure used by sorting, faceting, analytics, numeric/geo queries,
and vector search. Stored fields keep canonical source text for retrieval in
compressed chunks. Analyzed text has postings and stored retrieval but no
per-document value column; use a `string` variant when the same source value
must also sort or facet. Scalar types return their values from columns without
a second stored copy.

## Text analysis

A text analyzer is one tokenizer followed by a list of filters:

| Component | Meaning |
|---|---|
| `unicode_word` | Tokenizer: Unicode word segmentation (UAX#29). |
| `whitespace` | Tokenizer: split on whitespace. The default when an analyzer lists filters but no tokenizer. |
| `keyword` | Tokenizer: the whole value as one token. |
| `nfkc_cf` | Filter: NFKC case folding, normalized to a fixpoint. |
| `lowercase` | Filter: Unicode lowercasing only. |
| `fold` | Filter: accent and diacritic folding. |
| `english_possessive` | Filter: remove a trailing `'s`. |
| `kstem` | Filter: KStem English stemming; the only component with parameters. |

Prefer `nfkc_cf` to `lowercase` on Unicode-segmented text: bare lowercasing
leaves equal-looking strings unequal (composed versus decomposed forms, final
sigma), while NFKC case folding makes them compare equal.

`kstem` implements Bob Krovetz's dictionary-based English stemming algorithm:
`ponies` becomes `pony`, while recognized dictionary words are preserved or
replaced by their dictionary mapping. It runs at both index and query time
for `_t` fields. In custom analyzers, put it after `lowercase` or `nfkc_cf`,
and after `fold` if used. Tokens containing anything outside lowercase ASCII
letters, or with lengths outside 3-49 letters, pass through unstemmed.

`english_possessive` removes one trailing apostrophe followed by `s` or `S`,
matching Lucene's EnglishPossessiveFilter. It accepts ASCII apostrophes, right
single quotation marks (U+2019), and fullwidth apostrophes (U+FF07). Other
apostrophes and a bare trailing apostrophe are unchanged: `Winter's` becomes
`Winter`; `don't` and `dogs'` are left alone. The tokenizer may already discard
a trailing apostrophe before filtering.

Bare `"kstem"` includes equivalent possessive removal by default, stripping
the suffix before stemming within one filter stage. `_t` uses this default, so
`title_t:winter` matches `Winter's Tale`. To request stemming alone, disable
possessive removal explicitly:

```json
{"name": "kstem", "params": {"possessive": false}}
```

The standalone filter is useful without stemming. When placing it before
`kstem`, set `possessive: false` on KStem so removal happens only once.
Possessive removal applies even when the remaining word is non-ASCII or outside
KStem's length range. Source spelling, offsets, and positions are preserved.

Prefix, wildcard, regex, and fuzzy query terms receive the field's case and
accent folding without possessive removal or stemming. Neither
`english_possessive` nor `kstem` is available in STRING normalizers.

For Unicode text without English stemming, use `_un`, or configure
`unicode_word` with `["nfkc_cf", "fold"]` to retain accent folding as well.

## Field variants

A document supplies one value per logical field, and the schema can index that
value several ways. The primary keeps the field's name; each variant indexes
the same submitted value under `<field>__<label>`.

See [Documents and values](documents.md#field-variants) for the worked example:
searching author names by word and faceting on their whole values with the
built-in `_name` template. Its [schema definition](#templates-and-inheritance)
is shown below, followed by the rules for inheritance and customization.

### Variant names and constraints

Labels are ASCII: a letter first, then letters, digits, or single underscores.
`self` is reserved case-insensitively, and labels differing only by case
collide. Label lookup otherwise uses the authored case. `__` is reserved in
authored field names, template names, labels, and final document keys after
field mapping. It is only used in requests to select a representation.
`author_name__self` selects the primary; it does not create another physical
field.

Physical names are limited to 127 bytes. With variants, a template's maximum
instance name length is `127 - 2 - longest label length`. The `__self`
selector is not a physical name and does not consume that budget.

`multi`, `stored`, and `stored_resource` belong to the logical field. A variant
may not set them or declare nested `variants` or `defaults`. There is one stored
source on the primary when storage is enabled. Neither `id` nor `_version_`
may have variants, including inherited ones, and a variant cannot have type `id`.

The primary and every variant must use types from the same group:

| Group | Allowed types |
|---|---|
| Scalar | `text`, `string`, `int`, `float`, `double`, `date`. |
| Vector | `vector`; every branch must resolve to the same positive `dims`. |
| Geo | `geo_point`. |

### Default bindings for variants

For a field with variants, default bindings choose which representation to
use when a request names the field without a variant selector, such as
`author_name`. Fields without variants use the field itself; there is nothing
to configure.

With variants, `defaults.search` selects the representation for text search,
and `defaults.value` selects it for operations such as sorting and faceting.
Explicit bindings, including inherited ones, take precedence. An omitted
binding is inferred from the resolved field types:

| Primary type | Search binding | Value binding |
|---|---|---|
| `text` | `self` | Its sole `string` variant, or `self` if none exists. |
| `string` | Its sole `text` variant, or `self` if none exists. | `self` |
| Other types | `self` | `self` |

A text field with one string variant therefore searches words but sorts and
facets on whole values without a `defaults` block. A string field with one
text variant searches words through that variant and uses the primary for
whole-value operations.

If a text primary has multiple string variants, set `defaults.value`; if a
string primary has multiple text variants, set `defaults.search`. Otherwise,
the schema is rejected as ambiguous. Use `"self"` to keep an operation class
on the primary, or a variant label to select a particular representation.
The `_name` template explicitly sets `defaults.value` to `s`, preserving that
choice even if another string variant is added.

Inference uses types after inheritance. Each operation still checks the
chosen representation's index and column capabilities; it does not look for
an alternative if those checks fail.

| Operation | Bare field name uses |
|---|---|
| Match, phrase, simple query, expression terms and phrases, prefix, fuzzy, wildcard, regex | `defaults.search`, including a match inside a filter. |
| `any_of` / `:=`, ranges, field and range facets, sort, column expressions, metrics | `defaults.value`, then the operation's normal type and capability checks. |
| Exists (`f:*`), kNN, geo | The primary physical field. |
| Retrieval | The primary's source store, else its own typed column; never the value default. |

An explicit selector bypasses both bindings: `author_name__s` selects that
variant and `author_name__self` selects the primary. Exists on a bare name
tests primary presence; an explicit selector tests that representation, which
can differ for documents indexed before it was added.

A binding names `self` or a label in the field's effective variant map. See
[Searching](searching.md#field-bindings) for query and projection examples,
and use [`?explain=resolved`](http-api.md#explain-modes) to see which physical
field a request used.

### Templates and inheritance

Templates can carry variants and defaults. The default schema includes
`_name` and its multi-valued form `_names`:

```json
{
  "templates": {
    "_name": {
      "type": "text",
      "analyzer": {"tokenizer": "unicode_word", "filters": ["nfkc_cf", "fold"]},
      "variants": {"s": {"parent": "_s"}},
      "defaults": {"value": "s"}
    },
    "_names": {"parent": "_name", "multi": true}
  }
}
```

`author_name` uses `_name`; `author_name__s` selects its original whole-name
string variant. The root is resolved before the label, so the tail `_s` never
picks the default string template independently.

`_name` uses word search with case and accent folding, without English
stemming or possessive removal. Its string variant preserves the supplied
spelling for facets and sorting, so normalize names before indexing if you
need to.
A bare `type: text` would use the whitespace analyzer.

`variants` and `defaults` each inherit atomically: absent inherits, present
replaces the whole object, and `{}` clears it. There is no per-label merge.
For example, add fields to the
[`authors` collection](documents.md#field-variants) using the default `_name`
template:

```http
POST /collections/authors/_schema

{
  "fields": {
    "inherited": {
      "parent": "_name"
    },
    "replaced": {
      "parent": "_name",
      "variants": {
        "raw": "string"
      },
      "defaults": {}
    },
    "cleared": {
      "parent": "_name",
      "variants": {},
      "defaults": {}
    }
  }
}
```

`inherited` keeps `s` and the value binding. `replaced` has only `raw`; clearing
the inherited defaults lets its value binding infer `raw`.
`cleared` clears both the variants and their inherited defaults, leaving only
the primary. Within a present `defaults`, an omitted binding follows the
inference rules above. An empty `defaults: {}` restores inference; use an
explicit `"self"` binding to pin the primary.

A variant's own `parent` borrows physical settings only: type, index, column,
analyzer, normalizer, `long_terms`, and vector settings. It does not inherit
that parent's shape, storage, variants, or defaults. Parents name authored
fields or templates, never derived selectors.

A new suffix can inherit `_t`'s word analysis and add a whole-value variant.
Templates can be supplied when the collection is created:

```http
POST /collections/_create

{
  "name": "templates",
  "schema": {
    "templates": {
      "_title": {
        "parent": "_t",
        "variants": {
          "s": "string"
        }
      }
    }
  }
}
```

A field named `book_title` now indexes both `book_title` and `book_title__s`.
The `_title` template inherits `_t`'s case folding, accent folding, and English
stemming; its string variant keeps the whole value for sorting, faceting,
and exact lookup through the inferred value binding. See
[field retrieval](searching.md#field-retrieval-and-result-shape)
for retrieving a particular variant.

## STRING normalization and length

`normalizer` takes filters only: `lowercase`, `nfkc_cf`, and `fold`, using the
same component syntax as analyzer filters. Each input element stays one whole
value. Normalization applies at ingest and to query literals, facet `selected`
values, and range bounds, so a literal accepted at ingest finds its document
later. Absent `normalizer` inherits; `[]` clears it.

### Long terms

IDs, indexed STRING values after normalization, and analyzed TEXT tokens share
a 255-byte term space. Terms of 255 bytes or less stay unchanged. The
`long_terms` policy says what happens to longer ones; the default, `hash128`,
keeps long values distinct with no limit on value length:

| Policy | Over-limit term becomes |
|---|---|
| `hash128` (default) | The first 230 bytes, backed off to a UTF-8 boundary, followed immediately by 25 base36 characters hashing the whole term. |
| `truncate` | The first 255 bytes, backed off to a UTF-8 boundary. Values sharing that prefix merge, including IDs for overwrite and delete. |
| `reject` | Reject documents and requests containing over-limit terms. |

The `hash128` suffix is unseeded XXH3_128 of the whole term, taken as one
128-bit number from its canonical 16 bytes (high 64 bits then low 64 bits,
both big-endian) and written in `0-9` and `a-z`, most significant digit first,
zero padded to 25 digits.  A
300-byte value in a `_s` field therefore facets as its first 230 bytes plus a
25-character tail such as `...131pru8lxm5t1cohchv0tgjtg`.

What the policy means at query time:

- Exact queries (`any_of`, `:=`), range bounds, facet `selected` values,
  overwrite, and delete apply the same transform as ingest, so full values
  round-trip. TEXT exact membership applies it to the single analyzed term;
  match and phrase apply it per analyzed token.
- Different long values keep distinct IDs, exact matches, and facet buckets
  except for hash collisions.
- A prefix query longer than the kept prefix falls back to that prefix and
  returns a superset. Wildcard and regex queries do the same when their common
  leading literal prefix exceeds the limit; other patterns operate on the
  stored term bytes, hash suffix included. Fuzzy distance also compares
  transformed bytes, so similarity between discarded tails is not measured.
- The stored source, when enabled, keeps the full bytes before normalization
  or hashing and is used to return the exact value if its column representation
  was truncated.
- Column-only strings (`index: "none"`) have no term-space limit,
  but cannot serve field facets or exact queries.

Absent `long_terms` inherits through the parent chain, otherwise defaults to
`hash128`; an explicit policy overrides inheritance. Variants may set the policy
or borrow it through their own `parent`; they do not inherit it from their
logical primary. The policy applies only to ID, TEXT, and indexed STRING
representations.

## The operations

| Request | Meaning |
|---|---|
| `GET /collections/{c}/_schema` | the authored schema (valid write body) |
| `GET /collections/{c}/_schema?view=resolved` | effective representations, bindings, and schema coverage (HTTP only) |
| `POST /collections/{c}/_schema` | `mode=set` (default): set the named definitions, keep the rest |
| `POST /collections/{c}/_schema?mode=replace_all` | replace the whole schema |
| `POST /collections/_create` | create a collection, optionally with a `schema` installed before it becomes visible |

The same operations are available over gRPC as `luxir.Admin/GetSchema`,
`SetSchema`, and `CreateCollection`; see the [gRPC API](grpc.md).

### Resolved view

The default `GET`, also available as `?view=authored`, stays sparse: explicit
parents, variants, and defaults are returned without inheritance expansion.
The resolved view expands everything, per physical representation. After
adding the fields in [Templates and inheritance](#templates-and-inheritance),
inspect the `authors` collection:

```http
GET /collections/authors/_schema?view=resolved
```

```json
{
  "generation": 1,
  "fields": {
    "inherited": {
      "bindings": {
        "search": "inherited",
        "value": "inherited__s"
      },
      "representations": {
        "s": {
          "analyzer": null,
          "column": true,
          "coverage_complete": false,
          "index": "match",
          "introduced_generation": 1,
          "long_terms": "hash128",
          "multi": false,
          "name": "inherited__s",
          "normalizer": [],
          "oldest_generation": 0,
          "stored": false,
          "type": "string"
        },
        "self": {
          "analyzer": {
            "filters": ["nfkc_cf", "fold"],
            "tokenizer": "unicode_word"
          },
          "column": false,
          "coverage_complete": false,
          "index": "match",
          "introduced_generation": 1,
          "long_terms": "hash128",
          "multi": false,
          "name": "inherited",
          "normalizer": null,
          "oldest_generation": 0,
          "stored": true,
          "stored_resource": "_stored_",
          "type": "text"
        }
      }
    }
  }
}
```

(Abbreviated to the `inherited` field; the real response lists every declared
field.) The resolved view is diagnostic, not a schema write body. It contains:

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

Coverage is derived from segment schema generations, not from counting
documents that contain the field. Merges keep the minimum input generation and cannot establish
backfill. Deleting some documents from an old segment does not refine its
generation.

### Changes to a live collection

Schema changes apply to new requests and do not rewrite existing documents.

Adding a field or variant leaves older documents without its values. Adding
a variant can also change an inferred binding immediately. For a staged
rollout, explicitly pin the affected binding to its current representation
(for example, `"defaults": {"value": "self"}`) when adding the variant.
Reindex from your source data, then select the new variant explicitly or set
`defaults: {}` to restore inference for both bindings. To keep one binding
pinned, retain it in the `defaults` object and omit only the other. Older
documents without a new `author__s` variant will not match its exact queries
or appear in its facet buckets.

Schema edits are not checked for compatibility with existing data. To change
a field's type or analysis, use a new field or variant label and reindex.
Removing and reusing a name does not erase its old indexed values.

## Limits

- Schema edits do not rewrite existing documents; see
  [Changes to a live collection](#changes-to-a-live-collection).
- Analyzed text has no value column; use a string variant to sort text values
  or facet on whole values.
- `english_possessive` and `kstem` are analyzer filters only; STRING
  normalizers accept `lowercase`, `nfkc_cf`, and `fold`.
- There is no default geo suffix; geo fields need an explicit definition (see
  [Geo search](geo-search.md)).
