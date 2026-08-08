# Solux Features

Solux is a high-performance hybrid search engine: full-text relevance,
vector similarity, and faceted analytics in one native-code core. Requests
share one composable request tree - queries, filters, facets, statistics, and
fusion - served over gRPC and a JSON/HTTP API designed for humans.
Built from scratch for modern hardware and cloud economics, with a work
stealing task scheduler, async IO, and SIMD acceleration.

The ten-second version:

```
POST /collections/main/_search
{"query": {"match": {"title_w": "darkness"}}, "fields": ["id", "author_s", "year_i"], "get_number": true}
```

```json
{"found":1,"docs":[{"id":"1","author_s":"Le Guin","year_i":1969}]}
```

No schema defined up front, no collection created, no client library
installed. The [Quickstart](guide/quickstart.md) gets you here in a few
commands; the [architecture](design/architecture.md) explains why the
engine is built the way it is.

## Highlights

- **Hybrid search, natively.** A kNN query is a query like any other: it
  composes with boolean logic and filters in one tree, filters apply
  *inside* the vector search rather than after a fixed unfiltered top-k, and
  rank fusion combines lexical and vector sources in the same request.
- **One request, one round trip.** Top docs, facets with nested sub-ops,
  statistics, and fusion as named ops in a single request, executed in parallel
  over one index view.
- **Fast top-k that doesn't cheat.** Requests that don't require an exact
  count run with block-max pruning (MaxScore skipping); requests that require
  a count are counted exhaustively.
- **JSON you can type.** snake_case, untagged values, shorthands with
  exact structured equivalents, and unknown keys are errors with
  positions, not silence. `?explain=request` echoes your terse request
  back in canonical form - the API teaches the API.
- **Streaming everything.** NDJSON ingest with no stream-size or bulk-request
  ceiling (individual records and explicit atomic groups remain bounded); the
  server frames internal mini-batches. Responses are chunked, and long-lived
  HTTP and streaming gRPC connections use asynchronous network IO. Unary gRPC
  updates currently wait on the update worker before releasing their handler.
- **Frugal by design.** Native code, no garbage collector, and a
  memory-mapped index. One process is designed to scale up across a large
  modern machine.
- **Crash-safe commit structure.** Immutable segments and atomic commit
  points mean a crash reopens the previous published commit, never a
  half-published view.

## Schema and fields

- Schemaless start: field types inferred from name suffixes (`title_w`,
  `year_i`, `tags_ss`, `date_dt`, `embedding_v`, ...); no up-front schema
  required.
- Explicit schema API over HTTP/JSON and gRPC: field definitions, per-field
  analyzers, field inheritance (templates). `GET /collections/{c}/_schema`
  returns the authored schema and the output is itself a valid write body;
  `POST` sets the named definitions (`mode=set`, the default) or replaces the
  whole schema (`mode=replace_all` - the destructive operation must be typed,
  never implied by an HTTP verb). Reserved fields (`id`, `_version_`) are
  always materialized, so a replace cannot brick a collection.
- Field types: analyzed text, string, int, float, double, date (ISO-8601
  in, epoch-millis storage), id, vector, geo point (lat/lon;
  values ingest as `[lon, lat]` arrays, GeoJSON coordinate order,
  quantized to ~1cm).
- Per-field choices: index mode (match / range acceleration), multi-valued, column-stored (for sorting,
  faceting, and analytics), stored (for document retrieval; LZ4-compressed
  chunks).
- Floats and doubles are stored order-preserving, so numeric sorting and
  ranges over columns need no decode step.

## Text analysis

- Tokenizers: Unicode word segmentation (UAX#29), whitespace, and keyword.
  The common `unicode_word` plus `nfkc_cf` chain is fused internally into one
  analysis pass; it is not a separate tokenizer name in the schema.
- Token filters: NFKC case folding (normalized to a fixpoint), ASCII
  lowercase, accent/diacritic folding.

## Indexing and ingest

- Streaming ingest on both surfaces: gRPC bidirectional stream, and HTTP
  NDJSON with no stream-size limit - documents are parsed and indexed as bytes
  arrive. Clients do not need to choose a bounded bulk-request size; several
  streams are currently needed to saturate a many-core host. Individual HTTP
  records and explicit atomic groups remain bounded.
- Vector document values currently require the typed gRPC `Val.vec`/`arr_vec`
  arms; HTTP document arrays are not promoted to vector values yet. HTTP kNN
  query vectors are supported.
- Row documents on the current update path. The protobuf `columns` member is
  reserved but not consumed yet.
- Update/overwrite by id, delete by id, duplicate-allowed mode, and
  optional per-request atomicity (all-or-none with rollback).
- Collections auto-create on first write (can be disabled).
- Indexing memory is capped and flushes automatically; merges run in the
  background and never stall ingest or search.

## Queries

All query types are nodes in one tree and compose freely under boolean
clauses, as top-docs/fusion filters, and as fusion source queries.

- `match` (analyzed; AND/OR operator; minimum-match; over numeric fields,
  equality against the column), `boolean` (required / optional /
  prohibited / filter clauses, minimum-match), `phrase` (position-based,
  with optional slop measured as the spread of query-adjusted positions;
  reordered terms are allowed, an adjacent transposition costs 2, and the
  multi-value position gap of 100 can be crossed at slop 100 or more),
  `range` over numeric, date, and term-backed fields (`gte`/`gt`/`lte`/`lt`,
  any side open-ended; dates accept ISO-8601, epoch millis, or combined
  Solr/OpenSearch date math with request-stable `NOW`/`now`, evaluated in
  an optional request time zone (IANA or fixed offset), and a partial
  date means the window it names - equality on `2024-06-25` matches the
  whole day; string/text fields range over their indexed terms in byte
  order, constant-scoring; with no bounds it matches every document that
  has a value - a field-exists query),
  `prefix`, `fuzzy`, `constant_score`, match-all, `geo_box` (bounding-box
  over geo point fields, dateline-aware), `geo_distance` (inclusive radius
  in meters), and `knn` (vector search is just a query).
- Numeric, date, and geo fields declared with `index: "range"` build a points
  index that answers ranges, boxes, and whole-index range facets far
  faster than a column scan; without it the same queries still run off
  the column.
- Fuzzy matching rewrites to the closest terms with a default expansion cap of
  50. Callers can set `max_expansions`; a lower operator/clause-budget clamp is
  declared in response warnings. Blended scoring keeps rare misspellings from
  outranking the exact term, and fuzzy clauses participate in block-max pruning
  like any other clause.
- `simple_query`: a never-fails search-box syntax for end-user input
  (`+`/`-`, `|`, quoted phrases with an optional `~N` slop, grouping,
  trailing-`*` prefix, `~N` fuzzy terms, and `field:value` terms - including exact numeric and date
  matches like `price:10` or `created:2024-01-01`) - invalid syntax
  degrades to terms, never to an error.
- `expr`: the [query language](guide/query-language.md) for developers
  writing queries - a bare string anywhere the JSON API takes a query
  object.  Fielded terms and phrases (including strict quoted-phrase
  `~N` slop), AND/OR/NOT with real precedence,
  `+`/`-` prefixes, ranges (`year_i:[1960 TO 1970}`) and comparisons
  (`year_i:>=1960`), field groups (`title_w:(a OR b)`), and function forms for
  most structured query types (`fuzzy(smith, field=name_s, max_edits=2)`). The
  [structured query reference](guide/query-reference.md) lists fields and
  exceptions. Special characters only act
  in the position where they mean something, so `url_s:https://x` needs no
  escaping; `$vars` substitute request values without re-parsing them, so
  user input cannot inject syntax.  Strict grammar, byte-offset parse
  errors; degrading gracefully is `simple_query`'s job.
- Named non-scoring filters on top-docs and fusion sources - and a filter
  is where an expression string shines: `"filter": [{"name": "live",
  "query": "status_s:active AND year_i:>=1960"}]`.

## Search and ranking

- BM25 relevance scoring.
- Adaptive execution: requests that do not consume an exact total run with
  block-max pruning (per-block score bounds + MaxScore skipping); requests
  that ask for exact counts run exhaustively.
- Lexicographic sorting by column values, query score, and reader-local
  `(segment, docid)`, with ascending/descending directions, result limits, and
  field projection. Ordered page-after pagination is not implemented yet.
- Row- or column-oriented results per request (`document_format`): JSON
  defaults to row-oriented docs (missing field = absent key), gRPC to dense
  columns (missing = per-column sentinel) for analytics-friendly decoding.
- Multiple named search ops in one request, executed in parallel over the same
  index view; chunked streaming responses for large result sets.
- Opt-in execution profiles expose per-segment strategy, selection inputs,
  timing, and human-readable decisions for instrumented operations (currently
  string facets). Requests can also force single-threaded execution when
  isolating scheduler effects.
- Count-only and aggregate-only requests: `limit: 0` with `get_number: true`
  returns the exact count and any facets/metrics without fetching documents.
- Numeric `avg`, `sum`, `min`, and `max` operations at query level or per string-facet
  bucket.
- Hybrid fusion op: reciprocal rank fusion (RRF) over named sources (e.g.
  a lexical and a vector query), with shared and per-source filters.
- A search warnings channel: declared query-time degradations carry a code and
  message in the response rather than silently substituting query behavior.

## Facets and analytics

- Field (terms) facets over string, text, int, and date fields; range facets
  over int, float, double, and date fields.
- Date histograms: calendar gaps (day/week/month/quarter/year) stepped in
  a request-level or per-facet time zone (IANA or fixed offset), bounds
  accept date math (`NOW/DAY-30DAYS`), every bucket returned in order with
  zero counts included.
- Facet controls: limit, mincount, and missing bucket; string/ID facets can
  sort by one named metric sub-op.
- Nested sub-ops under string/ID facets: sub-facets and metrics per bucket.
- Counts are exact by default, never estimated.

## Vector search

- Dense float32 vector fields; the column store is the source of truth.
  Metrics: L2, inner product, cosine (with normalize-on-write by default).
- Exact kNN (full-precision column scan) and ANN via per-segment IVF+PQ
  indexes, mixed per segment by a size gate; approximate candidates are
  rescored at full precision.
- Filtered kNN applies filters inside the vector search and adaptively deepens
  ANN breadth instead of post-filtering a fixed unfiltered top-k. `exact: true`
  gives the true filtered top-k contract.
- Multi-valued vector fields with max-similarity collapse per document.
- ANN indexes are segment overlays: built at commit from the vector column and
  carried through merges automatically, without reindexing documents.
- Effort knobs: `k`, `nprobe`, `refine_candidates`, `min_scan_fraction`,
  and an `exact` switch.
- Deterministic execution: parallel vector search returns bit-identical
  results to serial.

## API surfaces

- gRPC: streaming search, unary and streaming update, schema admin,
  known-symbol server reflection, and health checks.
- HTTP/JSON: query, update (JSON and NDJSON), schema, health. The JSON is
  designed for humans: snake_case, untagged values, lowercase enum names,
  shorthands with exact structured equivalents.
- `?explain=request` echo mode: send the terse form, get back the
  canonical structured form.
- Strict validation everywhere: unknown keys are errors, not silence;
  depth and size limits are built in (the public port is treated as
  hostile).
- Request/response correlation ids on gRPC streams and updates; per-request
  freshness bound (`freshness_us`) on reads.

## Operations

- Storage: filesystem (memory-mapped reads) or in-memory.
- Crash-safe commits: files are written, synced, and atomically renamed; a
  crash lands on the previous commit point, never in between.
- Background merging with node-wide indexing-RAM budgeting.
- No built-in authentication or TLS yet: run the server inside your own
  network boundary.

## Status

Solux is pre-1.0 and moving fast; interfaces can change without
back-compat. It is currently a single-node engine with no replication,
distributed query execution, authentication, or TLS. This page lists shipped
capabilities; the [operations guide](guide/operations.md) states the deployment
boundary and current limitations.
