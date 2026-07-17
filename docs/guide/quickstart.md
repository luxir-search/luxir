# Quickstart

Solux speaks plain JSON over HTTP. Start the server, `curl` a document in,
`curl` a search out. There is no schema to define up front, no client library
to install, and no cluster to stand up first. This page gets you from nothing
to a working search in a few commands.

## Start the server

```bash
solux
```

That's it. The HTTP/JSON API is listening on port `9400`, storing data in
memory. To keep data across restarts, point it at a directory:

```bash
solux --store.backend=fs --store.data-dir=./data
```

Check it's alive:

```bash
curl http://localhost:9400/health
```

## Index your first document

```bash
curl -X POST http://localhost:9400/collections/main/_update \
  -H 'Content-Type: application/json' \
  -d '{"docs":[{"id":"1","title_w":"the left hand of darkness","author_s":"Le Guin","year_i":1969}],"commit":{}}'
```

```json
{"update_version":1,"status":"ok"}
```

You did not define a schema, and you did not create the `main` collection -
both just happened. **Field types come from the field name.** A `_w` suffix is
full-text (analyzed, tokenized), `_s` is an exact string, `_i` is an integer;
there are suffixes for floats, doubles, dates, and multi-valued versions of
each. Name a field `title_w` and it is searchable text; name it `year_i` and
it is a number you can range and sort on. Define an [explicit schema](schema.md)
later when you want control - you do not need one to start.

> **Reading the rest of this page:** examples below drop the `curl` wrapper and
> show just the method, path, and JSON body. To run one, wrap it:
> `curl -X POST http://localhost:9400<path> -H 'Content-Type: application/json' -d '<body>'`.

## Search

```
POST /collections/main/_query
{"query": {"match": {"title_w": "darkness"}}, "fields": ["id", "author_s", "year_i"]}
```

```json
{"found":1,"docs":[{"id":"1","author_s":"Le Guin","year_i":1969}]}
```

`match` analyzes your text the same way the field was indexed, so `darkness`
finds *"the left hand of darkness"*. `fields` chooses what comes back. A
document that doesn't have a requested field simply omits that key - a doc
object never carries `null` placeholders, so what you see is exactly what the
document has.

Prefer a uniform shape instead? Add `"document_format": "columns"` to the
request and every requested field appears in every doc, with an explicit
`null` where the document has no value - handy when feeding rows into a
table. (Over gRPC, responses are natively columnar; this setting picks the
placement there too.)

### Counts are exact

Add `get_number` and the response tells you exactly how many documents match,
not an estimate - even when you only page back a few:

```
POST /collections/main/_query
{"query": {"match": {"author_s": "Le Guin"}}, "fields": ["id"], "get_number": true, "limit": 2}
```

```json
{"found":3,"docs":[{"id":"1"},{"id":"2"}]}
```

Three match; you asked for two. `found` is the real total.

### Forgiving end-user search

For a search box where a human types whatever they want, use `simple_query`.
It parses operators, quotes, and field terms, and it never returns a parse
error - malformed input just does its best:

```
POST /collections/main/_query
{"query": {"simple_query": {"q": "darkness | earthsea", "fields": ["title_w"]}}, "fields": ["id"], "get_number": true}
```

```json
{"found":2,"docs":[{"id":"2"},{"id":"1"}]}
```

### The query language

When you're the one writing the query, a bare string anywhere a query object
goes is an expression in the [Solux query language](query-language.md):
fielded terms, AND/OR/NOT, ranges, and a function form that reaches every
query type. Unlike `simple_query`, malformed input is a parse error, not a
guess:

```
POST /collections/main/_query
{"query": "title_w:(darkness OR earthsea) AND year_i:[1960 TO 1970]", "fields": ["id"], "get_number": true}
```

```json
{"found":1,"docs":[{"id":"1"}]}
```

*"a wizard of earthsea"* matched the title group but has no `year_i`, so the
range clause excluded it.

## Bulk ingest: stream a whole file

Set the content type to `application/x-ndjson` and send one document per line.
The stream is unbounded - pipe in a file of any size and Solux indexes it as it
arrives, without buffering the whole thing:

```
POST /collections/main/_update      (Content-Type: application/x-ndjson)
{"id": "2", "title_w": "a wizard of earthsea"}
{"id": "3", "title_w": "the dispossessed"}
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

## Many collections, one endpoint

You never pre-create collections. Index to any name and it comes into existence
on first use:

```
POST /collections/books/_update
{"docs": [{"id": "a", "title_w": "dune"}], "commit": {}}
```

```json
{"update_version":1,"status":"ok"}
```

```
POST /collections/books/_query
{"query": {"match": {"title_w": "dune"}}, "fields": ["id"], "get_number": true}
```

```json
{"found":1,"docs":[{"id":"a"}]}
```

The same server holds as many collections as you like, each fully isolated.
(Auto-create is on by default; set `--no-ingest.auto-create-collection` if you'd
rather a write to an unknown collection be rejected.)

## Committing

Changes become visible on commit. You have three ways, use whichever fits:

- In a JSON update body: `"commit": {}`.
- On the URL: `POST /collections/main/_update?commit=true`.
- At the end of a stream: `{"_end_": {"commit": {}}}`.

## See what the server understood

Add `?explain=request` to a query and Solux echoes back the canonical request it
parsed - the shorthand you sent, expanded to the full form:

```
POST /collections/main/_query?explain=request
{"query": {"match": {"title_w": "dune"}}}
```

```json
{"collection":{"name":["main"]},"ops":{"q":{"top_docs":{"query":{"match":{"field":"title_w","val":"dune"}}}}}}
```

Handy for learning the API and for debugging a query that isn't matching what
you expect.

## Where to go next

- [Vector search](vector-search.md) - dense-vector and hybrid retrieval.
