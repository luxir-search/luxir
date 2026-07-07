# Solux Features

Solux is a high-performance hybrid search engine: full-text relevance,
vector similarity, and faceted analytics in one native-code core. Requests
are one composable tree - queries, filters, facets, statistics, and fusion
nest freely - served over gRPC and a JSON/HTTP API designed for humans.
Built from scratch for modern hardware and cloud economics, with a work
stealing task scheduler, async IO, and SIMD acceleration.

The ten-second version:

```
POST /collections/main/query
{"query": {"match": {"title_w": "darkness"}}, "fields": ["id", "author_s", "year_i"]}
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
  *inside* the vector search (k results means k results), and rank fusion
  combines lexical and vector sources in the same request.
- **One request, one round trip.** Top docs, facets with nested sub-ops,
  statistics, and fusion as named ops in a single request, executed in one
  parallel pass over the index.
- **Fast top-k that doesn't cheat.** Requests that don't require an exact
  count run with block-max pruning (MaxScore skipping); requests that require
  a count are counted exhaustively.
- **JSON you can type.** snake_case, untagged values, shorthands with
  exact structured equivalents, and unknown keys are errors with
  positions, not silence. `?explain=request` echoes your terse request
  back in canonical form - the API teaches the API.
- **Streaming everything.** NDJSON ingest with no size limit and no bulk
  batch to tune (the stream is the batch); chunked streaming responses;
  asynchronous network IO end to end.
- **Frugal by design.** Native code, no garbage collector, and a
  memory-mapped index. One process efficiently scales up to 
  the largest machine sizes.
- **Crash-safe by construction.** Immutable segments and atomic commit
  points: a crash lands on the previous commit, never in between.

## Schema and fields

- Schemaless start: field types inferred from name suffixes (`title_w`,
  `year_i`, `tags_ss`, `date_dt`, `embedding_v`, ...); no up-front schema
  required.
- Explicit schema API: field definitions, per-field analyzers,
  field inheritance, merge or replace semantics.
- Field types: analyzed text, string, int, float, double, date (ISO-8601
  in, epoch-millis storage), binary, id, vector.
- Per-field choices: indexed, multi-valued, column-stored (for sorting,
  faceting, and analytics), stored (for document retrieval; LZ4-compressed
  chunks).
- Floats and doubles are stored order-preserving, so numeric sorting and
  ranges over columns need no decode step.

## Text analysis

- Tokenizers: Unicode word segmentation (UAX#29), whitespace, keyword; a
  fused standard tokenizer combining segmentation with NFKC case folding.
- Token filters: NFKC case folding (normalized to a fixpoint), ASCII
  lowercase, accent/diacritic folding.

## Indexing and ingest

- Streaming ingest on both surfaces: gRPC bidirectional stream, and HTTP
  NDJSON with no size limit - documents are parsed and indexed as bytes
  arrive. Clients never need to batch for throughput.
- Row documents and columnar batches in the same update API.
- Update/overwrite by id, delete by id, duplicate-allowed mode, and
  optional per-request atomicity (all-or-none with rollback).
- Collections auto-create on first write (can be disabled).
- Indexing memory is capped and flushes automatically; merges run in the
  background and never stall ingest or search.

## Queries

All query types are nodes in one tree and compose freely - under boolean
clauses, under facet domains, as fusion sources, as filters.

- `match` (analyzed; AND/OR operator; minimum-match; over numeric fields,
  equality against the column), `boolean` (required / optional /
  prohibited / filter clauses, minimum-match), `phrase` (position-based),
  `range` over numeric and date fields (`gte`/`gt`/`lte`/`lt`, any side
  open-ended; dates accept ISO-8601 or epoch millis; with no bounds it
  matches every document that has a value - a field-exists query),
  `prefix`, `fuzzy`, `constant_score`, match-all, and `knn` (vector search
  is just a query).
- Fuzzy matching is complete by default: an expansion cap is explicit
  consent to truncation, and any truncation (including the operator
  backstop) is declared in the response warnings. Blended scoring keeps
  rare misspellings from outranking the exact term, and fuzzy clauses
  participate in block-max pruning like any other clause.
- `simple_query`: a never-fails search-box syntax for end-user input
  (`+`/`-`, `|`, quoted phrases, grouping, trailing-`*` prefix, `~N`
  fuzzy, and `field:value` terms - including exact numeric and date
  matches like `price:10` or `created:2024-01-01`) - invalid syntax
  degrades to terms, never to an error.
- `expr`: the [query language](guide/query-language.md) for developers
  writing queries - a bare string anywhere the JSON API takes a query
  object.  Fielded terms and phrases, AND/OR/NOT with real precedence,
  `+`/`-` prefixes, ranges (`year_i:[1960 TO 1970}`) and comparisons
  (`year_i:>=1960`), field groups (`title_w:(a OR b)`), and a function form
  that reaches every query type by its JSON name
  (`fuzzy(smith, field=name_s, max_edits=2)`).  Special characters only act
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
- Sorting by field values (ascending/descending, composable with score and
  doc order), offset/limit pagination, field projection with explicit
  `null` for missing values.
- Multiple named search ops in one request, executed in one parallel pass;
  chunked streaming responses for large result sets.
- Count-only and aggregate-only requests: `limit: 0` returns exact counts
  and any facets/stats without fetching documents.
- Hybrid fusion op: reciprocal rank fusion (RRF) over named sources (e.g.
  a lexical and a vector query), with shared and per-source filters.
- A warnings channel: declared degradations carry a code and message in
  the response - the engine never silently substitutes behavior.

## Facets and analytics

- Field (terms) facets over string, int, and date fields; range facets
  over int and date fields.
- Facet controls: limit, mincount, missing bucket, sort.
- Nested sub-ops under string facets: sub-facets and metrics per bucket.
- Counts are exact by default, never estimated.

## Vector search

- Dense float32 vector fields; the column store is the source of truth.
  Metrics: L2, inner product, cosine (with normalize-on-write by default).
- Exact kNN (full-precision column scan) and ANN via per-segment IVF+PQ
  indexes, mixed per segment by a size gate; approximate candidates are
  rescored at full precision.
- Filtered kNN applies filters inside the vector search - k results means
  k results, not k-minus-whatever-the-filter-removed.
- Multi-valued vector fields with max-similarity collapse per document.
- ANN indexes are segment overlays: built or rebuilt at commit without
  reindexing documents, carried through merges automatically.
- Effort knobs: `k`, `nprobe`, `refine_candidates`, `min_scan_fraction`,
  and an `exact` switch.
- Deterministic execution: parallel vector search returns bit-identical
  results to serial.

## API surfaces

- gRPC: streaming search, unary and streaming update, schema admin,
  server reflection, health checks.
- HTTP/JSON: query, update (JSON and NDJSON), health. The JSON is designed
  for humans: snake_case, untagged values, shorthands with exact
  structured equivalents.
- `?explain=request` echo mode: send the terse form, get back the
  canonical structured form.
- Strict validation everywhere: unknown keys are errors, not silence;
  depth and size limits are built in (the public port is treated as
  hostile).
- Request/response correlation ids; per-request freshness bound
  (`freshness_us`) on reads.

## Operations

- Storage: filesystem (memory-mapped reads) or in-memory.
- Crash-safe commits: files are written, synced, and atomically renamed; a
  crash lands on the previous commit point, never in between.
- Background merging with node-wide indexing-RAM budgeting.
- Graceful shutdown that drains in-flight requests and streams.
- No built-in authentication or TLS yet: run the server inside your own
  network boundary.

## Status

Solux is pre-1.0 and moving fast; interfaces can change without
back-compat. This page lists only what is shipped - if a capability is not
here, it is not in the engine yet. What is designed and coming next is
tracked on the project roadmap.
