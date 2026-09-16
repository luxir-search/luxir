# Luxir Features

Luxir is a native-code search engine with full-text relevance, vector
similarity, faceting, analytics, and geo search in one index. Queries,
filters, facets, statistics, and rank fusion go in one request, over a
JSON/HTTP API that is easy to write by hand and a gRPC API for programs. It
is native code written for modern hardware.

The simplest search is a URL:

```http
GET /collections/main/_search?query=title_t:kings
```

```json
{
  "docs": [
    {
      "id": "1",
      "author_name": "Brandon Sanderson",
      "price_f": 12.5,
      "series_s": "Stormlight",
      "title_t": "The Way of Kings",
      "year_i": 2010
    }
  ]
}
```

A JSON body takes the same fields and adds `ops`. This one returns documents,
counts by series, and the average price of all matches in one request:

```http
POST /collections/main/_search

{
  "query": {
    "match": {
      "author_name": "sanderson"
    }
  },
  "fields": ["id", "title_t", "price_f"],
  "limit": 2,
  "get_number": true,
  "ops": {
    "series": {
      "field_facet": {
        "field": "series_s"
      }
    },
    "average_price": "avg(price_f)"
  }
}
```

```json
{
  "found": 3,
  "docs": [
    {
      "id": "1",
      "title_t": "The Way of Kings",
      "price_f": 12.5
    },
    {
      "id": "2",
      "title_t": "Words of Radiance",
      "price_f": 15
    }
  ],
  "ops": {
    "average_price": 12,
    "series": {
      "buckets": [
        {
          "val": "Stormlight",
          "count": 2
        },
        {
          "val": "Mistborn",
          "count": 1
        }
      ]
    }
  }
}
```

That works with no schema or collection created ahead of time and no client
library. The [Quickstart](guide/quickstart.md) gets you here in a few
commands with these same documents, and the
[architecture](design/architecture.md) page explains why the engine is built
the way it is. This page lists what ships today.

## Performance

Native code with no garbage collector and no heap ceiling, built to use a
whole machine. Designed from the start for parallelism on modern hardware.

- Parallelism: A Work-stealing scheduler shared by indexing, merging, and search.
- Indexing is a pipeline of independent stages, so ingest never waits behind
  a merge and a commit does not stop the world. Send one NDJSON stream and
  indexing parallelizes across all available cores automatically.
- Merges parallelize inside a single merge, and increase the parallelism
  as long as indexing is under its RAM budget.
- Memory-mapped, zero-copy reads: the on-disk format is the in-memory format,
  so postings, columns, and vector indexes are consumed in place with no
  deserialization step.
- Asynchronous network IO on both surfaces. Cheap queries can run inline on the
  connection thread with no scheduler handoff, and a request can opt onto the
  shared scheduler so an expensive query does not occupy its connection.
- SIMD codecs for postings and numeric data (FastPFOR-based bit-packing,
  StreamVByte), roaring-style two-level bitsets for document sets, and BM25
  hot loops written to auto-vectorize.
- Block-max pruning for lightning fast top-k queries.
  Pareto frontiers in the index facilitate skipping whole groups of documents
  that won't be competitive.
- Sort pruning: numeric columns store per-block min and max zone maps. A
  field-sorted top-k skips whole blocks of values that can't be competitive,
  even without a points (BKD) index.
- Range queries without a points (BKD) index use the same zone maps: a numeric
  range or value set skips column blocks it cannot intersect, accepts whole
  blocks it covers, and decodes only the blocks that cross a bound.
- Adaptive facet counting picks among many strategies to best balance 
  performance and RAM usage.
- Filter cache ranked by rebuild cost per byte, so cheap filters are evicted
  before expensive ones.
- Allocation discipline: indexing runs on rollback-capable memory pools, and
  requests decode into arena-backed message objects with no per-field heap
  allocation on the way in.

## Schema and fields

- Field templates: built-in suffix rules (`title_t`, `year_i`, `tags_ss`, ...)
  let you start without defining your own schema. Templates also handle
  fields you cannot enumerate ahead of time, such as new product attributes
  in an ecommerce catalog. Define the rule once; new matching fields work
  automatically. [Templates](guide/schema.md#field-templates-for-dynamic-fields)
  can be customized and combined with explicit field definitions, which take
  precedence.
- Field types: analyzed text, string, int, float, double, date (ISO-8601 in,
  epoch-millis storage), id, vector, geo point.
- Explicit schema API over HTTP/JSON and gRPC. `GET` returns the authored
  schema, and the output is itself a valid write body; `POST` sets the named
  definitions (`mode=set`) or replaces the whole schema (`mode=replace_all`;
  the destructive form has to be spelled out rather than implied by an HTTP
  verb).
- Fields and templates can inherit settings with `parent`. The built-in
  `_name` and `_names` templates support people and titles:
  word search plus a whole-value variant for facets and sorting.
- Field variants: one input value indexed under several representations
  (`author` for word search, `author__s` for exact facets and sorts), with
  per-operation default bindings so a bare field name does the right thing.
- Per-field choices: index mode (match, or range acceleration), multi-valued,
  column-stored (sorting, faceting, analytics), stored source (retrieval, in
  LZ4-compressed chunks).
- Numeric, date, and geo fields with `index: "range"` build a points index;
  without it the same queries run off the column with the same results.
- Floats and doubles are stored order-preserving, so numeric sorting and
  ranges over columns need no decode step.
- Long terms: strings, tokens, and IDs over 255 bytes follow a per-field
  policy (hash the tail by default, truncate, or reject).

## Text analysis

- Tokenizers: Unicode word segmentation (UAX#29), whitespace, keyword.
- Token filters: NFKC case folding, ASCII lowercase, accent and diacritic
  folding, English possessive removal, and KStem English stemming (the
  dictionary-based Krovetz stemmer).
- The default `_t` field applies Unicode words, case folding, accent folding,
  possessive removal, and KStem. `_un` is Unicode text without stemming, `_u`
  keeps case and accents, and `_w`/`_wl` split on whitespace.
- Analyzer components carry typed parameters
  (`{"name": "kstem", "params": {"possessive": false}}`); string fields take a
  normalizer (filters only, applied to the whole value).
- Query-time analysis matches index-time analysis, so `match` finds what was
  indexed.

## Indexing and ingest

- Streaming ingest on both surfaces: HTTP NDJSON with no stream-size limit,
  and a gRPC bidirectional update stream. Individual records and explicit
  atomic groups are bounded; the stream is not.
- Stream grammar: `_update_` opens a group with options (`allow_dups`,
  `all_or_none`, `return_ids`, a different `collection`), `_end_` closes it
  and can commit. One connection can feed several collections.
- Field mapping at ingest (`field_map`, `drop_unmapped`): index a foreign dump
  as-is, renaming or dropping keys per request, with no file editing and no
  schema change.
- Update and overwrite by id, delete by id, a duplicate-allowed mode, and
  optional per-request atomicity (all-or-none with rollback).
- Per-document failures are reported by id and index with the same error
  object used everywhere else; the other documents in the request are still
  indexed.
- Collections auto-create on first write (can be disabled).
- Commits publish a new view: immediate, `commit_within_ms`, `?commit=true` on
  the URL, or at the end of a stream. Indexing memory is budgeted and flushes
  automatically; merges run in the background and never stall ingest or
  search.

## Queries

Every query type is a node in one tree and composes freely under boolean
clauses, as non-scoring filters, as facet domains, and as fusion sources.
Anywhere a query goes, it can be a structured object or an expression string.

- `match` (analyzed, AND/OR operator, minimum-match; numeric and date
  equality through columns), `phrase` (positional, with slop), `boolean`
  (required, optional, prohibited, and filter clauses with minimum-match),
  `any_of` (exact value-set membership), `exists`, and match-all.
- `range` over numeric, date, string, ID, and text fields, either side open.
  Date bounds accept ISO-8601, epoch millis, partial dates that mean the
  window they name (`2024-06` is all of June), and date math
  (`NOW/DAY-30DAYS`, `2024-06-25||+2d/d`) with a request-stable `NOW`,
  evaluated in a request time zone (IANA name or fixed offset).
- `prefix`, `wildcard` (`*` and `?`), `regex` (anchored whole-term), and
  `fuzzy` (edit distance up to 2, with blended scoring so a rare misspelling
  never outranks the exact term; fuzzy clauses take part in block-max pruning
  like any other clause).
- `constant_score`, `boost`, and `rescore`: keep a query's matches and replace
  each score with a value expression over columns and the original score
  (`score * def(popularity_i, 1)`).
- `knn` vector search, `geo_box`, and `geo_distance`.
- `simple_query`: a syntax for end-user input that never fails to parse (`+`/`-`, `|`,
  quoted phrases with `~N` slop, grouping, trailing-`*` prefix, `~N` fuzzy
  terms, and `field:value` terms against an allowlist).
- `expr`, the [query language](guide/query-language.md) for developers: a
  bare string anywhere the JSON API takes a query. Fielded terms and phrases,
  AND/OR/NOT with real precedence, `+`/`-`, ranges (`year_i:[1960 TO 1970}`)
  and comparisons (`year_i:>=1960`), exact values
  (`category_s:=(classic, fiction)`), field groups (`title_t:(a OR b)`),
  per-clause boosts, and function forms for most structured query types
  (`fuzzy(smith, field=name_s, max_edits=2)`; vector and geo queries stay
  structured). Special characters act only
  where they mean something, so most values need no escaping. Strict grammar
  with byte-offset errors; `$vars` substitute values without re-parsing them,
  so user input cannot inject operators.
- Both forms build the same query tree, so `?explain=request` returns the
  structured form of any request, expression strings included.
- Non-scoring filters on top-docs and fusion sources, routable past named
  sub-operations (`except_ops`). Expression strings are convenient as
  filters: `"filter": ["status_s:active AND year_i:>=1960"]`.

## Search and ranking

- BM25 relevance with block-max pruning (per-block score bounds plus MaxScore
  skipping) whenever the request does not consume an exact count; exhaustive
  counting when it does.
- Sorting by column values, query score, value expressions
  (`popularity_i + score * $weight`), and reader order; ascending or
  descending; lexicographic over several keys; `offset` and `limit` paging.
- Field projection: named fields, `*` wildcard patterns, or every retrievable
  field when `fields` is omitted. Row- or column-oriented documents per
  request (`document_format`): HTTP defaults to rows (a missing field is an
  absent key), gRPC to dense columns.
- Any number of named operations in one request over the same index view:
  ranked lists, facets, metrics, and fusion, nested to any depth and executed
  in parallel. HTTP responses preserve their names and nesting under `ops`.
  Root query shorthand returns `found` and `docs` directly in the HTTP envelope.
- Count-only and analytics-only requests: `limit: 0` with `get_number: true`
  loads no document fields.
- Stream every match: `limit: -1` with `?format=docs` returns one document
  per line over one connection, with no cursor, scroll state, or page size.
  The output pipes straight back into NDJSON ingest.
- Hybrid fusion: reciprocal rank fusion over named sources with shared and
  per-source filters, `limit` and `offset` over the fused list, and facets and
  metrics over the fused candidate set.
- A warnings channel: declared degradations (a clamped fuzzy distance, a
  skipped calendar bucket) carry a code and a message in the response instead
  of silently changing the query.
- Opt-in execution profiles (`profile: true`) with per-segment strategy,
  inputs, and timing for instrumented operations (string facets today), and
  `max_parallel` to force serial execution when isolating scheduler effects.

## Facets and analytics

- Field facets over string, ID, int, date, and text fields; range facets over
  int, float, double, and date fields with fixed or calendar gaps; query
  facets with arbitrary named query buckets.
- Date histograms: calendar gaps (day, week, month, quarter, year) stepped in
  a request or per-facet time zone, date-math bounds, every bucket returned in
  order with zero counts included, daylight-saving changes handled.
- Nesting: sub-facets under buckets, metrics per bucket, and a ranked
  `top_docs` (or `fusion`) list per bucket, all in one request.
- Expression metrics: `avg`, `sum`, `min`, and `max` over value expressions
  (`sum(price_f * qty_i) / sum(qty_i)`), at query level or per bucket; facets
  can sort by a named metric.
- Easy [multi-select navigation](guide/faceting.md#easy-multi-select-with-selected):
  put choices in each facet's `selected` array. Luxir builds the filters and
  automatically counts alternatives with the other facets' selections applied.
  `domain` overrides and `except_ops` routing allow custom filtering behavior.
- Facet controls: limit, mincount, and a missing bucket. Counts are exact by
  default.

## Vector search

- Dense float32 vector fields, single- or multi-valued; the column store is
  the source of truth, so there is no separate vector store to keep in sync
  and deletes, updates, and merges apply to vectors as they do to text.
  Metrics: L2, inner product, cosine (normalize-on-write by default).
- Exact kNN (full-precision column scan) and approximate search through
  per-segment IVF+PQ indexes, mixed per segment by a size gate. Approximate
  candidates are rescored at full precision straight from the column, so the
  recall recovery costs no extra storage.
- Filtered kNN applies filters inside the vector search and adaptively deepens
  ANN breadth instead of post-filtering. `exact: true` gives the exact
  filtered top-k, which also serves as the ground truth for measuring recall.
- Multi-valued vector fields collapse to one hit per document at its best
  similarity, with adaptive over-fetch so `k` documents still come back.
- ANN indexes are segment overlays: built at commit from the vector column and
  carried through merges automatically, without reindexing documents.
- Effort knobs: `k`, `refine_candidates`, `exact`, and per-engine knobs under
  `ivf` (`nprobe`, `min_scan_fraction`).
- Deterministic: parallel vector search returns bit-identical results to a
  serial run.

## Geo

- Geo point fields (`[lon, lat]` arrays in GeoJSON coordinate order,
  quantized to about a centimeter), single- or multi-valued, with an optional
  two-dimensional points index.
- Bounding-box queries (dateline-aware) and great-circle distance queries,
  usable as the main query, as a non-scoring filter, or as a boolean clause.

## API surfaces

- HTTP/JSON: search (GET with URL parameters for a bookmarkable search, or
  POST), update (JSON and NDJSON), schema, collection create, delete, and
  list, stats, and health. `?pretty` for humans; every body ends with a
  newline.
- gRPC: streaming search, unary and streaming update, schema and collection
  admin, stats, the standard health service, and known-symbol reflection.
  Search results are natively columnar.
- One vocabulary: both surfaces share the protobuf message model, so a shape
  learned over HTTP is the message a generated client sends.
- Strict validation everywhere: unknown keys are errors, nesting depth and
  sizes are bounded, and the public port is treated as hostile input.
- One error shape everywhere: `{kind, code, message}`, in-band and in HTTP
  error bodies and gRPC status details alike. `kind` fixes the transport
  status; `code` is the stable key.
- Request and response correlation ids, and a per-request freshness bound
  (`freshness_ms`) on reads.

## Operations

- Storage: filesystem (memory-mapped reads) or in-memory. Collections are
  independent indexes below one data directory.
- Crash-safe commits: files are written, synced, and atomically renamed, so a
  crash lands on the previous commit point. An optional checked-directory
  mode diagnoses filesystems that break the sync assumptions.
- One writer per data directory, enforced by a lock; `--read-only` nodes serve
  a directory that another process is writing.
- Background merging that parallelizes inside a single merge, admitted
  against a node-wide indexing-RAM budget. `--max-ram-mb` sizes the whole node
  from one number (25% of system or cgroup RAM by default).
- Operational statistics per node and per collection (`/_stats`): segments,
  live documents, bytes, merge activity, and cache counters.
- No built-in authentication or TLS yet: run the server inside your own
  network boundary.

## Status

Luxir is pre-1.0 and moving fast; interfaces can change without
back-compat. It is a single-node engine with no replication, distributed
query execution, authentication, or TLS. Benchmark results are not yet
published. This page lists shipped
capabilities; the [operations guide](guide/operations.md) states the
deployment boundary and the current limitations.
