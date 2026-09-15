# Indexing

Send documents in a JSON request, or stream them as NDJSON. A JSON request is
useful for a batch of documents, including a batch that must be atomic. An
NDJSON stream can carry a feed or a file of any size: documents are indexed
as the bytes arrive, with no bulk size to choose and no stream-size limit.
Either way, a write to a collection that does not exist yet creates it.

Both forms use:

```
POST /collections/{collection}/_update
```

## JSON updates

Send a bounded update as `application/json`:

```http
POST /collections/books/_update
Content-Type: application/json

{
  "request_id": "load-42",
  "docs": [
    {"id": "b1", "title_t": "Dune", "year_i": 1965},
    {"id": "b2", "title_t": "Dune Messiah", "year_i": 1969}
  ],
  "delete_ids": ["retired-book"],
  "return_ids": true,
  "commit": {}
}
```

```json
{
  "request_id": "load-42",
  "update_version": 1,
  "status": "ok",
  "ids": ["b1", "b2"]
}
```

One request can add documents, delete by id, and commit. The request fields
are:

| Field | Meaning |
|---|---|
| `request_id` | Opaque correlation value echoed in the response. |
| `docs` | Row-oriented JSON documents. |
| `delete_ids` | IDs to delete; an absent ID is a successful no-op. |
| `allow_dups` | Skip ID overwrite/deduplication. Default `false`. |
| `all_or_none` | Roll back the whole request if one document fails. Default `false`. |
| `return_ids` | Include successfully indexed IDs in request order. |
| `commit` | Make changes visible, with optional commit controls. |
| `field_map` | Rename input document keys onto schema fields for this request. |
| `drop_unmapped` | Drop doc keys not present in `field_map` instead of indexing them. |

Built-in field templates give `title_t` its text type and `year_i` its integer
type. You can also define fields explicitly or add templates for new groups
of fields; see [Documents and values](documents.md#field-templates-types-from-field-names).
Over gRPC the same request is `Indexer.Update` with row maps in `docs`; see
the [gRPC API](grpc.md).

## Unbounded NDJSON ingest

Set `Content-Type: application/x-ndjson` and put one document on each line:

```http
POST /collections/books/_update
Content-Type: application/x-ndjson

{"id":"b1","title_t":"Dune","year_i":1965}
{"id":"b2","title_t":"Dune Messiah","year_i":1969}
{"_end_":{"commit":{}}}
```

```json
{"update_version":2,"status":"ok"}
```

There is no stream-size limit. Luxir frames records as bytes arrive and
splits ordinary input into internal batches, so a client does not choose a
bulk size or hold a batch in memory. Size limits apply to a single record and
to an `all_or_none` group, not to the stream.

A single connection can have several internal batches indexing concurrently.
The server pauses reading when the limit on batches in flight is reached and
resumes as batches complete, keeping buffering bounded as the stream grows.

To send a file you already have, let `curl` stream it without reinterpreting
newlines:

```bash
curl -X POST 'http://localhost:9400/collections/books/_update?commit=true' \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary @books.ndjson
```

### Stream grammar

Three record forms make up a stream:

- A normal JSON object is a document.
- `{"_update_": {...}}` opens a group and supplies normal update options such
  as `request_id`, `allow_dups`, `all_or_none`, `return_ids`, `field_map`,
  `drop_unmapped`, or a collection override.
- `{"_end_": {...}}` closes the group and may commit. An empty object is a
  checkpoint that closes and reports the current group without ending the
  HTTP stream.

Options do not leak from one group into the next. For example, two
independently reported groups followed by one commit:

```http
POST /collections/books/_update
Content-Type: application/x-ndjson

{"_update_":{"request_id":"fiction","return_ids":true}}
{"id":"b1","kind_s":"fiction","title_t":"Dune"}
{}
{"_update_":{"request_id":"nonfiction"}}
{"id":"b2","kind_s":"nonfiction","title_t":"The Making of the Atomic Bomb"}
{"_end_":{"commit":{}}}
```

```jsonl
{"request_id":"fiction","update_version":3,"status":"ok","ids":["b1"]}
{"request_id":"nonfiction","update_version":4,"status":"ok"}
```

The response is NDJSON too, one update response per completed group. The URL
collection is the default; a control object can set
`"collection":"archive"` for the following group, so one connection can feed
several collections.

A stream may retain only the first 100 returned IDs and errors for a group;
indexing and `total_errors` are not capped by that reporting bound. A
request-level failure ends the stream: its response line carries
`status: "error"` and the `error` object alongside whatever the group had
already reported, and no later records from that connection are accepted.

The document-per-line [search export](searching.md#stream-every-match) uses
this same framing, and its `_header_` records are recognized and skipped, so
an export pipes straight back into `_update`. Export logical field names for
ingestion: explicit variant selectors are not document keys, and a retrieved
primary may have lost input that a variant needs, so a pipe is not a general
replacement for reindexing from the producer's source.

## Visibility and commits

An accepted update is searchable once a commit publishes a new index view.
Commit whichever way fits:

- In a JSON update body: `"commit": {}`.
- On the request URL, JSON or NDJSON: `POST /collections/main/_update?commit=true`.
- At the end of a stream: `{"_end_": {"commit": {}}}`.
- With no documents at all: `{"commit": {}}` as the whole body.

An empty commit object commits immediately and the response waits for
publication. `commit` may contain:

| Field | Meaning |
|---|---|
| `commit_within_ms` | Publish within this many milliseconds. `0` is immediate; a positive value lets the update response return before publication. |
| `build_aux_indexes` | Missing vector overlays to build, such as `["*"]` or `["vec.embedding_v"]`. Existing overlays are retained; empty requests no builds. |
| `wait_for_merges` | Wait for in-flight merges before publishing. |
| `max_segments` | Force the committed data down to at most this many segments before returning. `0` means no forced merge. |

On the HTTP path, `?commit=true` guarantees the request is published before
it completes: a JSON body commits immediately (`commit_within_ms` is forced
to `0`; its other commit options still apply), and an NDJSON stream commits
at the end of the stream.

Commits are crash-safe: segments are immutable and a commit point is
published atomically, so a crash reopens the previous commit rather than a
half-published view. Frequent forced merges are expensive; `max_segments` is
an explicit maintenance action, not a normal ingest setting. See
[Operating Luxir](operations.md#visibility-policy) for choosing a commit
interval.

## IDs, overwrites, and deletes

By default, indexing a document whose `id` already exists replaces the old
document. Replacement is whole-document replacement, not a field patch: a
field omitted by the new version is absent from the new document.

Set `allow_dups: true` for append-only data where duplicate ID values are
intentional. That turns off overwrite semantics for the request; it does not
change how other requests behave.

Delete by ID with no document body:

```http
POST /collections/books/_update

{"delete_ids":["b1","b2"],"commit":{}}
```

```json
{"update_version":2,"status":"ok"}
```

IDs over 255 bytes follow the field's
[`long_terms` policy](schema.md#string-normalization-and-length); overwrite
and delete apply the same transform as indexing, so a long ID still names
one document.

## Field mapping

`field_map` renames input keys at ingest, so an existing NDJSON dump can be
indexed without rewriting the file or changing the schema. Put the mapping on
the URL:

```bash
curl -X POST 'http://localhost:9400/collections/books/_update?field_map=bookId:id,headline:title_t&drop_unmapped=true&commit=true' \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary @books.ndjson
```

Each `from:to` entry indexes input key `from` under schema field `to`. The
last `:` in an entry splits it, so input keys may contain colons; an empty
target (`internal_notes:`) drops that key. The parameter repeats
(`?field_map=a:b&field_map=c:d`) if one value gets long; input keys
containing commas need the body form below. `drop_unmapped=true` drops every
key not in the map, so only the mapped keys index. That includes `id`, so map
your id key explicitly (`id:id` if the input already uses that name).

Keys not in the map index under their own name by default. After mapping, a
name appearing more than once in a document keeps the last occurrence, the
same last-wins rule as a duplicated key. Mapping applies to top-level keys of
each document; it does not flatten nested objects.

The same two knobs are fields of the update request itself (and of a
streaming `_update_` control object, where they apply to that group's
documents):

```http
POST /collections/books/_update

{
  "docs": [{"bookId": "b1", "headline": "Dune"}],
  "field_map": {"bookId": "id", "headline": "title_t"},
  "drop_unmapped": true,
  "commit": {}
}
```

If a request or group sets either `field_map` or `drop_unmapped`, the URL
values for both are ignored for it; otherwise the URL values apply. The
mapping is per request and is not stored with the collection, so the same
dump can be mapped differently on each load.

Targets are logical field names, such as `author_name`, rather than variant
selectors or template names. External keys containing `__` can be renamed or
dropped before indexing. Each surviving key fans out once through its schema
[variants](schema.md#field-variants). Using the `authors` collection from
[Documents and values](documents.md#field-variants):

```http
POST /collections/authors/_update

{
  "docs": [
    {
      "bookId": "mapped",
      "external__author": "Octavia Butler",
      "discard__key": "ignored"
    }
  ],
  "field_map": {
    "bookId": "id",
    "external__author": "author_name",
    "discard__key": ""
  },
  "commit": {}
}
```

This supplies `author_name` once and populates both `author_name` and
`author_name__s`.

## Per-document failures

Unless `all_or_none` is set, valid documents are indexed even if others fail.
A failed document leaves any older version with the same ID intact:

```json
{
  "update_version": 9,
  "status": "partial",
  "errors": [
    {
      "id": "b2",
      "index": 1,
      "error": {
        "kind": "invalid_request",
        "code": "unknown_field",
        "message": "Field not found: nosuffix"
      }
    }
  ],
  "total_errors": 1
}
```

Each entry names the document by `id` and by `index` in the request and
carries the standard [error object](http-api.md#errors).
`total_errors` counts every failed document; it exceeds the length of
`errors` only when a transport retained a prefix of them.

A value rejected by any [variant](schema.md#field-variants) fails the entire
document.

The response status is:

- `ok`: every attempted change succeeded.
- `partial`: some documents succeeded and some failed.
- `error`: no document took effect, or a request-level failure occurred. Only
  request-level failures set the top-level `error` object.

With `all_or_none: true`, processing stops at the first bad document and rolls
back documents and deletes already applied by that request. Atomicity covers
one update request or one explicit NDJSON group; it never spans independent
requests.

Always inspect the response body. Per-document failures are a valid HTTP
exchange, so the HTTP status code does not express the update result.

## Atomic groups are bounded

Rollback requires retaining the whole atomic unit, so an atomic group cannot
be unbounded. An NDJSON group with
`all_or_none: true` is therefore accumulated as one bounded request and is
subject to `--indexing.max-request-body`. Ordinary streaming groups remain
unbounded.

The relevant server controls are:

- `--indexing.max-request-body` for buffered JSON bodies and atomic groups
  (default `32MB`).
- `--indexing.max-record` for one NDJSON record (defaults to the buffered-body
  limit).
- `--indexing.stream-batch-size` and `--indexing.stream-batch-docs` for
  internal non-atomic handoff granularity.
- `--indexing.max-inflight-batches` for the number of internal batches a
  connection can submit concurrently (`0` selects an automatic limit based
  on task-arena concurrency; `1` makes batch submission serial).

## Limits

- Delete-by-query and field-level partial updates are not implemented.
- Each admitted update message pins the schema in force at admission, and a
  schema change never rewrites or backfills existing documents; see
  [changes to a live collection](schema.md#changes-to-a-live-collection).
