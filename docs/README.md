# Luxir Documentation

New here? The [Quickstart](guide/quickstart.md) goes from an empty server to
indexed documents, full-text queries, exact counts, and streaming import/export
with `curl`. The [features page](features.md) is the compact answer to "what
does Luxir do today?"

## Use Luxir

1. [Quickstart](guide/quickstart.md) - the shortest path to a useful search.
2. [Documents and values](guide/documents.md) - field naming, missing values,
   IDs, coercion, multi-valued shapes, and retrieval.
3. [Indexing](guide/indexing.md) - updates, overwrite and delete semantics,
   commits, failures, atomic batches, and unbounded NDJSON streams.
4. [Searching](guide/searching.md) - query forms, filters, results, counts,
   sorting, multiple operations, metrics, and streaming export.
5. [Structured query reference](guide/query-reference.md) - every query arm,
   field, default, and composition rule.
6. [Query language](guide/query-language.md) - strict developer expressions
   and injection-safe variables.
7. [Schema and fields](guide/schema.md) - suffix-based defaults, explicit
   fields, analyzers, storage, and indexes.
8. [Faceting](guide/faceting.md) - terms facets, range/date histograms, nested
   facets, and per-bucket metrics.
9. [Vector and hybrid search](guide/vector-search.md) - exact and ANN kNN,
   in-search filtering, and reciprocal-rank fusion.
10. [Geo search](guide/geo-search.md) - point indexing, bounding boxes, distance
   queries, and dateline behavior.
11. [Dates and time zones](guide/dates.md) - date values, date math, civil-time
   queries, and calendar buckets.
12. [HTTP API conventions](guide/http-api.md) - endpoints, framing, strict
    JSON, partial results, and error behavior.
13. [gRPC API](guide/grpc.md) - services, streaming model, and transport
    differences from HTTP.
14. [Operating Luxir](guide/operations.md) - persistence, ports, memory and
    ingest controls, commits, and the current security/availability boundary.

## Understand or contribute

- [Architecture](design/architecture.md) explains how the engine is put
  together and why: immutable segments, mmap, work stealing, adaptive query
  execution, and native vector integration.
- [Vector search design](design/vector-search.md) covers segment overlays and
  the ANN execution model.
- [Codebase map](dev/codebase-map.md) traces source components and request flow.
- [Build setup](dev/build-setup.md) covers toolchains, dependencies, presets,
  and runtime time-zone data.

Rule of thumb: a page that assumes a source checkout belongs in `dev/` or
`design/`. Pages in `guide/` start from a running server and a client.
