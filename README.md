# Luxir

Luxir is a high-performance hybrid search engine: full-text relevance, vector
similarity, faceting, and analytics in one native-code core. Queries, filters,
facets, metrics, and rank fusion are one composable request tree, served over
an HTTP/JSON API for humans and a typed streaming gRPC API for applications.

```http
POST /collections/books/_search
Content-Type: application/json

{"query":"title_t:(dune OR messiah) AND year_i:>=1965",
 "fields":["id","title_t","year_i"],"get_number":true}
```

```json
{
  "found": 2,
  "docs": [
    {
      "id": "b1",
      "title_t": "Dune",
      "year_i": 1965
    },
    {
      "id": "b2",
      "title_t": "Dune Messiah",
      "year_i": 1969
    }
  ]
}
```

No collection creation, schema, or client library is required to get
there. Built-in field templates provide useful defaults. Templates also
handle new field names as your data grows, and can be combined with explicit
field definitions. A write to a new collection creates it, and a
terse request can always be echoed back in its canonical form.

## Why Luxir

Luxir is a new search engine from Yonik Seeley, the original author of Apache
Solr and a longtime Lucene/Solr committer.

- Native code, designed for high throughput and efficient memory use, with parallel
  indexing, merging, and search.
- Text and vector search work together, with shared filters and built-in
  rank fusion.
- One request can return several result lists, facets, and statistics, all
  over the same view of the index.
- Queries can be structured JSON or protobuf, or expressions such as
  `title_t:dune AND year_i:>=1965`. Variables let you reuse a query with
  different input without constructing or escaping query strings.
- Multi-select faceting handles filtering and facet counts automatically:
  send the chosen values in `selected`. Facets can also contain metrics,
  nested facets, and top matching documents.
- Stream any number of documents in or out. Load a dataset in one request,
  retrieve all matches, or pipe results into another collection or server.

The complete shipped capability list is in [Luxir Features](docs/features.md).
For the design rationale, see [Architecture](docs/design/architecture.md).

## Try it

Download a single binary from <https://luxir.org/download/>,
start it, and check the HTTP endpoint:

```bash
./luxir
curl http://localhost:9400/health
```

Index a document and commit it:

```bash
curl -X POST http://localhost:9400/collections/books/_update \
  -H 'Content-Type: application/json' \
  -d '{
    "docs": [
      {
        "id": "b1",
        "title_t": "The Way of Kings",
        "author_s": "Sanderson",
        "year_i": 2010
      }
    ],
    "commit": {}
  }'
```

Search it:

```bash
curl -X POST 'http://localhost:9400/collections/books/_search?pretty' \
  -H 'Content-Type: application/json' \
  -d '{"query":{"match":{"title_t":"kings"}},"fields":["id","author_s","year_i"]}'
```

Continue with the [Quickstart](docs/guide/quickstart.md), then use the
[documentation map](docs/README.md) to go deeper.

## Build from source

Releases with binaries are published on the [download page](https://luxir.org/download/).
Build with the [development container](docs/dev/container-build.md) or a
[native toolchain](docs/dev/build-setup.md). The container supplies Ubuntu 22.04,
GCC 16.2, CMake, and the pinned normal/ASan dependencies. After building or
importing the image as described in the container guide:

```bash
./tools/dev-container cmake --preset container-release
./tools/dev-container cmake --build --preset container-release
./build/container-release/bin/luxir
```

The resulting executable can run directly on a supported Linux host.

The project is licensed under the [Apache License 2.0](LICENSE). Third-party
components and their licenses are listed in [NOTICE](NOTICE).
See [CONTRIBUTING.md](CONTRIBUTING.md) for how to report problems and
contribute, and [SECURITY.md](SECURITY.md) for reporting vulnerabilities.

## Current scope

Luxir is pre-release software. The HTTP and gRPC APIs, configuration, and the
on-disk index format change without notice, and there is no compatibility
guarantee before 1.0: expect to reindex when upgrading. It is currently a
single-node engine with no built-in authentication or TLS; deploy it behind
your own network and security boundary. Replication, distributed query
execution, and packaged clients are not shipped
yet. Binary releases target Linux on x86-64 currently.
See more in the [operations guide](docs/guide/operations.md).
