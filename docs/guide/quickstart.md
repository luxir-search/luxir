# Quickstart

Luxir speaks plain JSON over HTTP. Start the server, `curl` a document in,
`curl` a search out. There is no schema to define up front, no client library
to install, and no cluster to stand up first. Once the source build is ready,
this page gets you to a working search in a few commands.

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

```bash
curl -X POST http://localhost:9400/collections/main/_update \
  -H 'Content-Type: application/json' \
  -d '{"docs":[
        {"id":"1","title_t":"The Way of Kings","author_s":"Sanderson","year_i":2010},
        {"id":"2","title_t":"Words of Radiance","author_s":"Sanderson"},
        {"id":"3","title_t":"Mistborn: The Final Empire","author_s":"Sanderson","year_i":2006}
      ],"commit":{}}'
```

```json
{"update_version":1,"status":"ok"}
```

You did not define a schema, and you did not create the `main` collection -
both just happened. **Field types come from the field name.** A `_t` suffix is
full-text (analyzed, tokenized), `_s` is an exact string, `_i` is an integer;
there are suffixes for floats, doubles, dates, and multi-valued versions of
each. Name a field `title_t` and it is searchable text; name it `year_i` and
it is a number you can range and sort on. Define an [explicit schema](schema.md)
later when you want control - you do not need one to start.

> **Reading the rest of this page:** requests are shown as HTTP: method, path,
> and body. On the website, every request block has a **Copy as curl** button
> that copies the runnable command, with `?pretty` added to the URL so the
> output reads well in a terminal. Reading the Markdown source, wrap one
> yourself as above:
> `curl -X POST 'http://localhost:9400<path>?pretty' -H 'Content-Type: application/json' -d '<body>'`.

## Search

```http
POST /collections/main/_search

{"query": {"match": {"title_t": "kings"}}, "fields": ["id", "author_s", "year_i"], "get_number": true}
```

```json
{"found":1,"docs":[{"id":"1","author_s":"Sanderson","year_i":2010}]}
```

Add `?pretty` to the search URL (`/collections/main/_search?pretty`) for
readable output:

```json
{
  "found": 1,
  "docs": [
    {
      "id": "1",
      "author_s": "Sanderson",
      "year_i": 2010
    }
  ]
}
```

Alternatively, pipe the curl response through `| jq`.

`match` analyzes your text the same way the field was indexed, so `kings`
finds *"The Way of Kings"*. `fields` chooses what comes back. A
document that doesn't have a requested field simply omits that key - a doc
object never carries `null` placeholders, so what you see is exactly what the
document has.

Prefer a uniform shape instead? Add `"document_format": "columns"` to the
request and every supported projected field appears in every doc, with an
explicit `null` where the document has no value - handy when feeding rows into
a table. (Over gRPC, responses are natively columnar; this setting picks the
placement there too.)

### Counts are exact

Add `get_number` and the response tells you exactly how many documents match,
not an estimate - even when you only page back a few:

```http
POST /collections/main/_search

{"query": {"match": {"author_s": "Sanderson"}}, "fields": ["id"], "get_number": true, "limit": 2}
```

```json
{"found":3,"docs":[{"id":"1"},{"id":"2"}]}
```

Three match; you asked for two. `found` is the real total.

### Forgiving end-user search

For a search box where a human types whatever they want, use `simple_query`.
It parses operators, quotes, and field terms, and it never returns a parse
error - malformed input just does its best:

```http
POST /collections/main/_search

{"query": {"simple_query": {"q": "kings | radiance", "fields": ["title_t"]}}, "fields": ["id"], "get_number": true}
```

```json
{"found":2,"docs":[{"id":"2"},{"id":"1"}]}
```

### The query language

When you're the one writing the query, a bare string anywhere a query object
goes is an expression in the [Luxir query language](query-language.md):
fielded terms, AND/OR/NOT, ranges, and function forms for most structured query
types. Unlike `simple_query`, malformed input is a parse error, not a guess:

```http
POST /collections/main/_search

{"query": "title_t:(kings OR radiance) AND year_i:[2010 TO 2020]", "fields": ["id"], "get_number": true}
```

```json
{"found":1,"docs":[{"id":"1"}]}
```

*"Words of Radiance"* matched the title group but has no `year_i`, so the
range clause excluded it.

## Bulk ingest: stream a whole file

Set the content type to `application/x-ndjson` and send one document per line.
The stream is unbounded - pipe in a file of any size and Luxir indexes it as it
arrives, without buffering the whole thing:

```http
POST /collections/main/_update
Content-Type: application/x-ndjson

{"id": "4", "title_t": "Oathbringer", "author_s": "Sanderson", "year_i": 2017}
{"id": "5", "title_t": "The Well of Ascension", "author_s": "Sanderson", "year_i": 2007}
{"_end_": {"commit": {}}}
```

```json
{"update_version":2,"status":"ok"}
```

Lines starting with `_update_` or `_end_` are control objects, not documents.
`_update_` opens a group and sets options for the documents that follow
(`allow_dups`, `all_or_none`, a different `collection`, ...); `_end_` closes the
group and can commit. Everything in between is just documents. To index an
NDJSON file you already have:

```bash
curl -X POST http://localhost:9400/collections/main/_update \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary @books.ndjson
```

## Bulk export: stream every match, no cursor

Add `?format=docs` to a query and the response is bare NDJSON documents - one
per line, no envelope, no paging. `limit: -1` means every match, streamed over
one connection; there is no scroll API or cursor token to manage:

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

Ask for `get_number` and a `_header_` line leads the stream so tools know the
total up front: `{"_header_":{"found":5}}`. Execution warnings, when there are
any, also arrive in a `_header_` - degraded execution is never silent. Header
lines are recognized (and skipped) by ingest, so export pipes straight back
into `/_update`:

```bash
curl -s 'http://localhost:9400/collections/main/_search?format=docs' \
     -H 'Content-Type: application/json' \
     -d '{"query": {"all": true}, "limit": -1, "fields": ["id", "title_t"]}' |
curl -X POST 'http://localhost:9400/collections/backup/_update?commit=true' \
     -H 'Content-Type: application/x-ndjson' --data-binary @-
```

If anything fails mid-stream, the chunked response ends without its
terminator, so HTTP clients report truncation instead of quietly delivering a
partial result. A cleanly finished body is a complete result set.

## Many collections, one endpoint

You never pre-create collections. Index to any name and it comes into existence
on first use:

```http
POST /collections/books/_update

{"docs": [{"id": "a", "title_t": "Dune"}], "commit": {}}
```

```json
{"update_version":1,"status":"ok"}
```

```http
POST /collections/books/_search

{"query": {"match": {"title_t": "dune"}}, "fields": ["id"], "get_number": true}
```

```json
{"found":1,"docs":[{"id":"a"}]}
```

The same server holds multiple collections as independent index namespaces.
They share the process scheduler and memory, and Luxir does not currently
provide per-collection tenant quotas or authorization boundaries. Auto-create
is on by default; set `--no-indexing.auto-create-collection` if a write to an
unknown collection should be rejected.

## Committing

Changes become visible on commit. You have three ways, use whichever fits:

- In a JSON update body: `"commit": {}`.
- On an NDJSON request URL: `POST /collections/main/_update?commit=true`.
- At the end of a stream: `{"_end_": {"commit": {}}}`.

## See what the server understood

Add `?explain=request` to a query and Luxir echoes back the canonical request it
parsed - the shorthand you sent, expanded to the full form:

```http
POST /collections/main/_search?explain=request

{"query": {"match": {"title_t": "dune"}}}
```

```json
{"collection":"main","ops":{"q":{"top_docs":{"query":{"match":{"field":"title_t","val":"dune"}}}}}}
```

Handy for learning the API and for debugging a query that isn't matching what
you expect.

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
