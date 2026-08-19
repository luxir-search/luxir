# Indexing

Luxir has one update model with two HTTP encodings. Use a JSON request when a
group of documents is naturally bounded or must be atomic. Use NDJSON for a
feed or file of any size. The latter is a real stream: documents enter the
indexing pipeline while later bytes are still arriving, so a client does not
have to invent a user-visible bulk size.

Both forms use:

```
POST /collections/{collection}/_update
```

A write to a missing collection creates it by default. Collection names are a
single path component and names beginning with `_` are reserved. Disable
automatic creation with `--no-ingest.auto-create-collection` when collection
names must be provisioned elsewhere.

## JSON updates

Send a bounded update as `application/json`:

```http
POST /collections/books/_update
Content-Type: application/json

{
  "request_id": "load-42",
  "docs": [
    {"id": "b1", "title_w": "dune", "year_i": 1965},
    {"id": "b2", "title_w": "dune messiah", "year_i": 1969}
  ],
  "delete_ids": ["retired-book"],
  "return_ids": true,
  "commit": {}
}
```

```json
{"request_id":"load-42","update_version":1,"status":"ok","ids":["b1","b2"]}
```

The request fields are:

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

The protobuf request contains a reserved `columns` member, but the current
update handler does not consume it. Send row maps through `docs`; do not
populate `columns`. See [gRPC API](grpc.md).

## Field mapping

`field_map` reshapes a foreign document stream at ingest, so an existing NDJSON
dump indexes as-is: no editing the file, no schema change. Point Luxir at the
file and put the mapping on the URL:

```bash
curl -X POST 'http://localhost:9400/collections/books/_update?field_map=bookId:id,headline:title_w&drop_unmapped=true&commit=true' \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary @books.ndjson
```

Each `from:to` entry indexes input key `from` under schema field `to`. The last
`:` in an entry splits it, so input keys may contain colons; an empty target
(`internal_notes:`) drops that key. The parameter repeats
(`?field_map=a:b&field_map=c:d`) if one value gets long; input keys containing
commas need the body form below. `drop_unmapped=true` drops every key not in
the map, so only the mapped keys index -- note that includes `id`, so map your
id key explicitly (`id:id` if the input already uses that name).

Keys not in the map index under their own name by default. After mapping, a
name appearing more than once in a document keeps the last occurrence, the same
last-wins rule as a duplicated key. Mapping applies to top-level keys of each
document; it does not flatten nested objects.

The same two knobs are fields of the update request itself (and of a streaming
`_update_` control object, where they apply to that group's documents):

```http
POST /collections/books/_update
Content-Type: application/json

{
  "docs": [{"bookId": "b1", "headline": "dune"}],
  "field_map": {"bookId": "id", "headline": "title_w"},
  "drop_unmapped": true,
  "commit": {}
}
```

A request or group that sets either knob owns both, and the URL default is
ignored for it; otherwise the URL values apply. The mapping is a property of
the load, not the collection: the same dump can be re-shaped differently per
request.

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

Delete-by-query and field-level partial updates are not implemented.

## Visibility and commits

An accepted update is not searchable until a commit publishes a new index
view. An empty commit object commits immediately and the response waits for
publication:

```json
{"commit":{}}
```

`commit` may contain:

| Field | Meaning |
|---|---|
| `commit_within_us` | Publish within this many microseconds. `0` is immediate; a positive value lets the update response return before publication. |
| `build_aux_indexes` | Missing vector overlays to build, such as `["*"]` or `["vec.embedding_v"]`. Existing overlays are retained; empty requests no builds. |
| `wait_for_merges` | Wait for in-flight merges before publishing. |
| `max_segments` | Force the committed data down to at most this many segments before returning. `0` means no forced merge. |

Commit without adding documents by sending `{"commit":{}}`. On the NDJSON
HTTP path, `?commit=true` is also available as a convenient end-of-stream
commit. Frequent forced merges are expensive; `max_segments` is an explicit
maintenance action, not a normal ingest setting.

## Per-document failures

Unless `all_or_none` is set, document validation failures do not poison their
neighbors. A failed document has no effect and an older version with the same
ID remains intact:

```json
{
  "update_version": 9,
  "status": "partial",
  "errors": [
    {"id": "b2", "error_message": "...", "index": 1}
  ]
}
```

The response status is:

- `ok`: every attempted change succeeded.
- `partial`: some documents succeeded and some failed.
- `error`: no document took effect, or a request-level/commit failure occurred.

With `all_or_none: true`, processing stops at the first bad document and rolls
back documents and deletes already applied by that request. Atomicity covers
one update request or one explicit NDJSON group; it never spans independent
requests.

Always inspect the response body. Per-document failures are a valid HTTP
exchange and therefore do not rely on the HTTP status code to express the
update result.

## Unbounded NDJSON ingest

Set `Content-Type: application/x-ndjson` and put one document on each line:

```http
POST /collections/books/_update
Content-Type: application/x-ndjson

{"id":"b1","title_w":"dune","year_i":1965}
{"id":"b2","title_w":"dune messiah","year_i":1969}
{"_end_":{"commit":{}}}
```

There is no stream-size limit. Luxir frames records as bytes arrive and cuts
ordinary non-atomic input into internal mini-batches. The limits are on one
record and on explicitly atomic material, not on the stream.

Three record forms make up the stream grammar:

- A normal JSON object is a document.
- `{"_update_": {...}}` opens a group and supplies normal update options such
  as `request_id`, `allow_dups`, `all_or_none`, `return_ids`, `field_map`,
  `drop_unmapped`, or a collection override.
- `{"_end_": {...}}` closes the group and may commit. An empty object is a
  checkpoint that closes and reports the current group without ending the
  HTTP stream.

The URL collection is the default. A control object can set
`"collection":{"name":["archive"]}` for the following group, which lets one
connection feed several collections. Options do not leak from one group into
the next.

For example, two independently reported groups followed by one commit:

```ndjson
{"_update_":{"request_id":"fiction","return_ids":true}}
{"id":"b1","kind_s":"fiction","title_w":"dune"}
{}
{"_update_":{"request_id":"nonfiction"}}
{"id":"b2","kind_s":"nonfiction","title_w":"the making of the atomic bomb"}
{"_end_":{"commit":{}}}
```

The response is NDJSON too, one update response per completed group. A stream
may retain only the first 100 returned IDs and errors for a group; counts and
indexing are not capped by that reporting bound.

To send an existing file without letting `curl` reinterpret newlines:

```bash
curl -X POST http://localhost:9400/collections/books/_update \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary @books.ndjson
```

The document-per-line [search export](searching.md#stream-every-match) is valid
input here. Its `_header_` records are recognized and skipped, so collections
can be copied with a pipe and no format conversion.

## Atomic streams are deliberately bounded

An unbounded stream and all-or-none atomicity cannot both be promised: rollback
requires retaining the atomic unit. An NDJSON group with `all_or_none: true` is
therefore accumulated as one bounded request and is subject to
`--ingest.max-request-body`. Ordinary streaming groups remain unbounded.

The relevant server controls are:

- `--ingest.max-request-body` for buffered JSON bodies and atomic groups
  (default `32MB`).
- `--ingest.max-record` for one NDJSON record (defaults to the buffered-body
  limit).
- `--ingest.stream-batch-size` and `--ingest.stream-batch-docs` for internal
  non-atomic handoff granularity.

At present a single HTTP NDJSON connection pipelines storage and indexing but
does not fan several internal batches into the engine concurrently. Use
several input streams when maximum ingest throughput matters. This is a
throughput detail, not a size limit or a client-visible batch contract.
