# Luxir Documentation

Luxir is a native-code search engine with full-text relevance, vector
similarity, faceting, analytics, and geo search in one index. It has a JSON
API over HTTP and a gRPC API. Built-in field templates mean you can start
without writing a schema, and they also cover field names that show up later
in your data. Writing to a new collection creates it, and one request can
return the documents, facets, and statistics for a whole page of results.

New here? The [Quickstart](guide/quickstart.md) goes from an empty server to
indexed documents, full-text queries, exact counts, and streaming import and
export with `curl`. The [features page](features.md) lists everything Luxir
does today.

The main design points:

- **One request returns a whole results page.** Documents, facets,
  statistics, and hybrid fusion are named operations in the same request, all
  computed over the same view of the index. Counts are exact by default.
- **Text and vector search work together.** A vector search is just another
  query. It takes the same filters, applied inside the search rather than
  after it, and the same facets and metrics. Rank fusion combines lexical and
  vector results in the same request.
- **Native code.** No garbage collector, no heap ceiling, memory-mapped
  segments, and a work-stealing scheduler, so one process can use a whole
  machine.

Requests are structured JSON (protobuf over gRPC). Anywhere a request takes a
query, you can write a query-language string instead, and `?explain=request`
shows the structured form of what you sent. Parse errors include the
position. Ingest and export both stream, with no size limit and no scroll
state, and commits are crash-safe on immutable segments.

## Use Luxir

1. [Quickstart](guide/quickstart.md) - a few `curl` commands to a working
   search.
2. [Documents and values](guide/documents.md) - field templates, explicit fields,
   missing values, IDs, coercion, arrays, and what comes back.
3. [Indexing](guide/indexing.md) - JSON updates, NDJSON streams of any size,
   commits, overwrites and deletes, field mapping, and per-document errors.
4. [Searching](guide/searching.md) - the request shape: queries, filters,
   exact counts, sorting, several operations in one request, and streaming
   export.
5. [Structured query reference](guide/query-reference.md) - every query arm,
   field, default, and composition rule.
6. [Query language](guide/query-language.md) - the developer's query string:
   real precedence, almost no escaping, functions, and injection-safe
   variables.
7. [Schema and fields](guide/schema.md) - explicit fields, templates for dynamic
   fields, variants and bindings, analyzers and normalizers, storage, and indexes.
8. [Faceting](guide/faceting.md) - field, range, date, and query facets;
   nested facets, metrics, top documents per bucket, and multi-select.
9. [Vector and hybrid search](guide/vector-search.md) - exact and approximate
   kNN, filters inside the search, and reciprocal rank fusion.
10. [Geo search](guide/geo-search.md) - points, bounding boxes, distance
    queries, and the dateline.
11. [Dates and time zones](guide/dates.md) - date values, date math, civil
    time, and calendar buckets.
12. [HTTP API conventions](guide/http-api.md) - endpoints, framing, strict
    JSON, explain modes, and errors.
13. [gRPC API](guide/grpc.md) - services, streams, and how the typed surface
    maps onto the JSON one.
14. [Operating Luxir](guide/operations.md) - persistence, ports, memory
    budgets, read-only nodes, and the current security and availability
    boundary.

15. [Index replication](guide/replication.md) - following a source, discovery,
    reserved snapshots, resumable downloads, and installation status.

## Reference

- [Protobuf API reference](reference/protobuf.md) - messages, fields, enums,
  and RPCs generated from the API definitions.

## Understand or contribute

- [Architecture](design/architecture.md) explains how the engine is put
  together and why: immutable segments, mmap, work stealing, adaptive query
  execution, and native vector integration.
- [Vector search design](design/vector-search.md) covers segment overlays and
  the ANN execution model.
- [Codebase map](dev/codebase-map.md) traces source components and request
  flow.
- [Build setup](dev/build-setup.md) covers toolchains, dependencies, presets,
  and runtime time-zone data.
- [Container build](dev/container-build.md) provides the shared Ubuntu 22.04
  compiler and dependency environment, including ASan.
- [Building a release](dev/releases.md) covers branches, tags, CPU tiers,
  validation, and binary packaging.
- [Documentation](dev/documentation.md) covers editing the guides and
  regenerating the API reference.

Rule of thumb: a page that assumes a source checkout belongs in `dev/` or
`design/`. Pages in `guide/` start from a running server and a client.
