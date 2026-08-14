# Luxir

Luxir is a high-performance hybrid search engine: full-text relevance, vector
similarity, faceting, and analytics in one native-code core. Queries, filters,
facets, metrics, and rank fusion are one composable request tree, served over
an HTTP/JSON API for humans and a typed streaming gRPC API for applications.

```http
POST /collections/books/_search
Content-Type: application/json

{"query":"title_w:(dune OR messiah) AND year_i:>=1965",
 "fields":["id","title_w","year_i"],"get_number":true}
```

```json
{"found":2,"docs":[{"id":"b1","title_w":"dune","year_i":1965},{"id":"b2","title_w":"dune messiah","year_i":1969}]}
```

No collection creation, schema ceremony, or client library is required to get
there. Field-name suffixes provide useful defaults, a write to a new collection
creates it, and a terse request can always be echoed back in its canonical form.

## Why Luxir

- **Hybrid is a property of the query tree, not a pipeline.** kNN composes with
  boolean logic and filters, and reciprocal-rank fusion combines lexical and
  vector rankings in the same request.
- **One request describes the page.** Return ranked documents, exact counts,
  facets with nested sub-facets, and numeric metrics in one round trip.
- **The API is meant to be written.** JSON values are untagged, names are
  `snake_case`, shorthands have exact structured equivalents, and unknown keys
  are errors rather than ignored typos.
- **Large transfers are streams, not cursor protocols.** Feed unbounded NDJSON
  to the update endpoint and export every match as document-per-line NDJSON over
  one connection.
- **Efficiency is the product.** Cloud makes inefficiency a recurring bill.
  Luxir is built to use one large modern machine well: native code, memory-mapped
  immutable segments, work-stealing parallelism, and SIMD-aware data paths.

The complete shipped capability list is in [Luxir Features](docs/features.md).
For the design rationale, see [Architecture](docs/design/architecture.md).

## Try it

Start a built server and check the HTTP endpoint:

```bash
./build/gcc-release/bin/luxir
curl http://localhost:9400/health
```

Index a document and commit it:

```bash
curl -X POST http://localhost:9400/collections/books/_update \
  -H 'Content-Type: application/json' \
  -d '{"docs":[{"id":"b1","title_w":"the left hand of darkness","author_s":"Le Guin","year_i":1969}],"commit":{}}'
```

Search it:

```bash
curl -X POST http://localhost:9400/collections/books/_search \
  -H 'Content-Type: application/json' \
  -d '{"query":{"match":{"title_w":"darkness"}},"fields":["id","author_s","year_i"]}'
```

Continue with the [Quickstart](docs/guide/quickstart.md), then use the
[documentation map](docs/README.md) to go deeper.

## Build from source

Luxir is pre-1.0 and currently distributed as source. It requires a
C++26-capable compiler, CMake, Ninja, vcpkg, and the native dependencies listed
in [Build Setup](docs/dev/build-setup.md). With those dependencies installed:

```bash
cmake --preset gcc-release
cmake --build --preset gcc-release
./build/gcc-release/bin/luxir
```

The project is licensed under the [Apache License 2.0](LICENSE).

## Current scope

Luxir is moving quickly and interfaces can change. It is currently a
single-node engine with no built-in authentication or TLS; deploy it behind
your own network and security boundary. Replication, distributed query
execution, packaged clients, and a collection-management API are not shipped
yet. Source builds also assume a prepared Linux/GCC/vcpkg environment; there is
no packaged binary or turnkey clean-machine installer yet. The
[operations guide](docs/guide/operations.md) covers the production boundary
honestly.
