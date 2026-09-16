# Quickstart

Luxir has a JSON API over HTTP. This page starts the server, indexes a few
documents with `curl`, and searches them. No schema, client library, or
cluster setup is needed first. It then covers exact counts, facets and
metrics in the same request, `simple_query` for search boxes, the query
language, and streaming import and export of files of any size.

## Get Luxir

Download the release for your platform from <https://luxir.org/download/>,
unpack it, and put the `luxir` binary on your `PATH`. Releases are built for
Linux on x86-64. If you would rather build it yourself, or want to work on the
engine, [Build Setup](../dev/build-setup.md) covers the toolchain and presets;
the result is `build/gcc-release/bin/luxir`.

## Start the server

```bash
luxir
```

That's it. The HTTP/JSON API is listening on port `9400`, storing data in
memory. To keep data across restarts, point it at a directory:

```bash
luxir --store.backend=fs --store.data-dir=./data
```

Check it's alive:

```bash
curl http://localhost:9400/health
```

## Index documents

Index three books with sample prices in dollars:

```bash
curl -X POST http://localhost:9400/collections/main/_update \
  -H 'Content-Type: application/json' \
  -d '{
    "docs": [
      {
        "id": "1",
        "title_t": "The Way of Kings",
        "author_name": "Brandon Sanderson",
        "series_s": "Stormlight",
        "year_i": 2010,
        "price_f": 12.5
      },
      {
        "id": "2",
        "title_t": "Words of Radiance",
        "author_name": "Brandon Sanderson",
        "series_s": "Stormlight",
        "year_i": 2014,
        "price_f": 15.0
      },
      {
        "id": "3",
        "title_t": "Mistborn: The Final Empire",
        "author_name": "Brandon Sanderson",
        "series_s": "Mistborn",
        "year_i": 2006,
        "price_f": 8.5
      }
    ],
    "commit": {}
  }'
```

```json
{"update_version":1,"status":"ok"}
```

The write created the `main` collection, and **built-in field templates**
supplied the field types: `_t` is searchable text, `_name` is a name that
supports word search plus whole-name facets and sorting, `_s` is an exact
string, `_i` is an integer you can range and sort on, and `_f` stores
floating-point numbers. You did not need to define your own schema.

Templates also handle fields you do not know about yet. For example, a custom
`_attr` template can cover new product attributes as an ecommerce catalog
grows. You can combine templates with explicit field definitions and choose
your own names. See [Schema](schema.md#field-templates-for-dynamic-fields).

`author_name` is one input indexed two ways: `author_name:sanderson` matches a
word of the name, while a facet or sort on `author_name` uses the whole value
`Brandon Sanderson`. [Field variants](documents.md#field-variants) shows how
the template does this and how to define your own.

## Search

The simplest search is a URL:

```bash
curl 'http://localhost:9400/collections/main/_search?pretty&query=title_t:kings'
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

`title_t:kings` searches the title field for `kings`, using the same text
analysis as indexing, so it finds *"The Way of Kings"*. `query` is an
expression in the [Luxir query language](query-language.md). `pretty` formats
the response for reading; leave it off and a program gets one compact line.
The same URL works in a browser, and it can be bookmarked or shared. `fields`,
`sort`, `limit`, and the other common request fields have
[URL parameter forms](searching.md#url-request-field-overlay) too.

Requests with more structure, such as facets and metrics, are JSON bodies.
The rest of this page uses them.

> **Reading the rest of this page:** requests are shown as HTTP: method, path,
> and body. On the website, every request block has a **Copy as curl** button
> that copies the runnable command, with `?pretty` added to the URL so the
> output reads well in a terminal. Reading the Markdown source, wrap one
> yourself as above:
> `curl -X POST 'http://localhost:9400<path>?pretty' -H 'Content-Type: application/json' -d '<body>'`.

The same search as JSON, choosing which fields come back and asking for the
exact match count:

```http
POST /collections/main/_search

{
  "query": "title_t:kings",
  "fields": ["id", "title_t", "author_name", "year_i"],
  "get_number": true
}
```

```json
{
  "found": 1,
  "docs": [
    {
      "id": "1",
      "title_t": "The Way of Kings",
      "author_name": "Brandon Sanderson",
      "year_i": 2010
    }
  ]
}
```

The equivalent [structured form](query-reference.md#match) of the query is
`"query": {"match": {"title_t": "kings"}}`. You can use either form anywhere
a query is accepted.

`fields` chooses what comes back; without it, every retrievable field is
returned. A document that doesn't have a requested field omits that key; no
`null` placeholder is written.

To get the same keys in every doc, add `"document_format": "columns"` to the
request. Every supported projected field then appears in every doc, with an
explicit `null` where the document has no value, which is convenient when
feeding rows into a table. (Over gRPC, responses are natively columnar; this
setting picks the placement there too.)

### Counts are exact

Add `get_number` and `found` is the total number of matching documents,
regardless of `limit`:

```http
POST /collections/main/_search

{
  "query": {"match": {"author_name": "sanderson"}},
  "fields": ["id", "title_t"],
  "get_number": true,
  "limit": 2
}
```

```json
{
  "found": 3,
  "docs": [
    {
      "id": "1",
      "title_t": "The Way of Kings"
    },
    {
      "id": "2",
      "title_t": "Words of Radiance"
    }
  ]
}
```

`found` is 3 even though `limit` was 2.

### Forgiving end-user search

For a search box where a human types whatever they want, use `simple_query`.
It understands operators, quotes, and field terms, and it never returns a
parse error, so malformed input still runs as a search:

```http
POST /collections/main/_search

{
  "query": {
    "simple_query": {"q": "kings | radiance", "fields": ["title_t"]}
  },
  "fields": ["id", "title_t"],
  "get_number": true
}
```

```json
{
  "found": 2,
  "docs": [
    {
      "id": "2",
      "title_t": "Words of Radiance"
    },
    {
      "id": "1",
      "title_t": "The Way of Kings"
    }
  ]
}
```

### The query language

When you're the one writing the query, a bare string anywhere a query object
goes is an expression in the [Luxir query language](query-language.md):
fielded terms, AND/OR/NOT, ranges, and function forms for most structured query
types:

```http
POST /collections/main/_search

{
  "query": "title_t:(kings OR radiance) AND year_i:[2010 TO 2013]",
  "fields": ["id", "title_t"],
  "get_number": true
}
```

```json
{
  "found": 1,
  "docs": [
    {
      "id": "1",
      "title_t": "The Way of Kings"
    }
  ]
}
```

*"Words of Radiance"* matched the title group, but its `year_i` is `2014`,
outside the requested range of `2010` through `2013`.

### Facets and metrics, in the same request

The same request can return books, count them by series, and calculate their
average price. This example also includes the lowest price per series:

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
        "field": "series_s",
        "ops": {
          "lowest_price": "min(price_f)"
        }
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
          "count": 2,
          "lowest_price": 12.5
        },
        {
          "val": "Mistborn",
          "count": 1,
          "lowest_price": 8.5
        }
      ]
    }
  }
}
```

Operations nest: `lowest_price` runs once per series bucket, while
`average_price` summarizes all three matches, including the book beyond
`limit: 2`. One round trip returns the documents, facet counts, and aggregate
metrics over one consistent view of the index.
[Faceting](faceting.md) covers range and date buckets, nested facets, and top
documents per bucket.

## Bulk ingest: stream a whole file

Set the content type to `application/x-ndjson` and send one document per line.
The stream is unbounded - pipe in a file of any size and Luxir indexes it as it
arrives, without buffering the whole thing. `?commit=true` commits at the end
of the stream, making the documents searchable before the request completes:

```http
POST /collections/main/_update?commit=true
Content-Type: application/x-ndjson

{"id": "4", "title_t": "Oathbringer", "author_name": "Brandon Sanderson", "series_s": "Stormlight", "year_i": 2017, "price_f": 17.5}
{"id": "5", "title_t": "The Well of Ascension", "author_name": "Brandon Sanderson", "series_s": "Mistborn", "year_i": 2007, "price_f": 10.0}
```

```json
{"update_version":2,"status":"ok"}
```

To index an NDJSON file you already have:

```bash
curl -X POST 'http://localhost:9400/collections/main/_update?commit=true' \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary @books.ndjson
```

The optional `_update_` and `_end_` control records let you group updates and
set options within a stream. See [Indexing](indexing.md#unbounded-ndjson-ingest).

## Bulk export

Add `?format=docs` to a query and the response is NDJSON, one document per
line with no envelope. `limit: -1` returns every match, streamed over one
connection, so there is no scroll API or cursor token to manage:

```http
POST /collections/main/_search?format=docs

{"query": {"all": true}, "limit": -1, "fields": ["id", "title_t"]}
```

```jsonl
{"id":"1","title_t":"The Way of Kings"}
{"id":"2","title_t":"Words of Radiance"}
{"id":"3","title_t":"Mistborn: The Final Empire"}
{"id":"4","title_t":"Oathbringer"}
{"id":"5","title_t":"The Well of Ascension"}
```

With `get_number`, a `_header_` line starts the stream so a consumer knows
the total before reading the documents: `{"_header_":{"found":5}}`.
Execution warnings, when there are any, also arrive in a `_header_` line.
Ingest recognizes and skips header lines, so an export can be piped straight
back into `/_update`:

```bash
curl -s 'http://localhost:9400/collections/main/_search?format=docs' \
     -H 'Content-Type: application/json' \
     -d '{"query": {"all": true}, "limit": -1, "fields": ["id", "title_t"]}' |
curl -X POST 'http://localhost:9400/collections/backup/_update?commit=true' \
     -H 'Content-Type: application/x-ndjson' --data-binary @-
```

If anything fails mid-stream, the chunked response ends without its
terminating chunk, so HTTP clients report a truncated body. A response that
terminates normally is complete.

## Multiple collections

Collections do not need to be created in advance. The first update to a new
collection name creates it:

```http
POST /collections/books/_update

{"docs": [{"id": "a", "title_t": "Dune"}], "commit": {}}
```

```json
{"update_version":1,"status":"ok"}
```

```http
POST /collections/books/_search

{
  "query": {"match": {"title_t": "dune"}},
  "fields": ["id", "title_t"],
  "get_number": true
}
```

```json
{
  "found": 1,
  "docs": [
    {
      "id": "a",
      "title_t": "Dune"
    }
  ]
}
```

The same server holds multiple collections as independent index namespaces.
They share the process scheduler and memory, and Luxir does not currently
provide per-collection tenant quotas or authorization boundaries. Auto-create
is on by default; set `--no-indexing.auto-create-collection` if a write to an
unknown collection should be rejected.

## Committing

Changes become visible on commit. You have three ways, use whichever fits:

- In a JSON update body: `"commit": {}`.
- On the request URL, JSON or NDJSON: `POST /collections/main/_update?commit=true`.
- At the end of a stream: `{"_end_": {"commit": {}}}`.

## See what the server understood

Add `?explain=request` to a query and Luxir echoes back the canonical request it
parsed - the shorthand you sent, expanded to the full form:

```http
POST /collections/main/_search?explain=request

{"query": {"match": {"title_t": "dune"}}}
```

```json
{
  "collection": "main",
  "ops": {
    "q": {
      "top_docs": {
        "query": {
          "match": {
            "field": "title_t",
            "val": "dune"
          }
        }
      }
    }
  }
}
```

This is useful for learning the API and for debugging a query that isn't
matching what you expect: type the short form, read back the full one, and
you have the request your code should generate.

Use [`?explain=resolved`](http-api.md#explain-modes) to inspect which physical
fields a request uses. It returns `request` and `resolved_fields`, and runs
ordinary preparation without collecting results. Post back the `request`
member to execute it.

## Where to go next

- [Documents and values](documents.md) - field naming, IDs, nulls, arrays, and coercion.
- [Indexing](indexing.md) - update, delete, commit, error, and stream semantics.
- [Searching](searching.md) - request and response shapes, filters, ops, and metrics.
- [Structured queries](query-reference.md) - every query arm and option.
- [Faceting](faceting.md) - field facets, range facets, and nested analytics.
- [Vector search](vector-search.md) - dense-vector and hybrid retrieval.
- [Geo search](geo-search.md) - bounding boxes and distance queries.
- [HTTP conventions](http-api.md) - framing, validation, and error behavior.
- [Operating Luxir](operations.md) - persistence, resource controls, and security.
