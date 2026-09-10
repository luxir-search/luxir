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
automatic creation with `--no-indexing.auto-create-collection` when collection
names must be provisioned elsewhere.

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

Send row maps through `docs`. See [gRPC API](grpc.md).

## Field mapping

`field_map` reshapes a foreign document stream at ingest, so an existing NDJSON
dump indexes as-is: no editing the file, no schema change. Point Luxir at the
file and put the mapping on the URL:

```bash
curl -X POST 'http://localhost:9400/collections/books/_update?field_map=bookId:id,headline:title_t&drop_unmapped=true&commit=true' \
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

Targets must be logical field names. A variant selector (`author__s`), primary
selector (`author__self`), or abstract template name is an invalid target and
fails the request. An external key containing `__` can be renamed or dropped;
only the final non-dropped document keys must obey the logical naming rule.
Each surviving key fans out once through its schema variants.

The same two knobs are fields of the update request itself (and of a streaming
`_update_` control object, where they apply to that group's documents):

```http
POST /collections/books/_update
Content-Type: application/json

{
  "docs": [{"bookId": "b1", "headline": "Dune"}],
  "field_map": {"bookId": "id", "headline": "title_t"},
  "drop_unmapped": true,
  "commit": {}
}
```

A request or group that sets either knob owns both, and the URL default is
ignored for it; otherwise the URL values apply. The mapping is a property of
the load, not the collection: the same dump can be re-shaped differently per
request.

For the `names` collection from [Schema](schema.md#field-variants):

```http
POST /collections/names/_update

{
  "docs": [{"bookId":"mapped","external__author":"Octavia Butler","discard__key":"ignored"}],
  "field_map": {"bookId":"id","external__author":"author","discard__key":""},
  "commit": {}
}
```

This supplies `author` once and populates both `author` and `author__s`.

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
| `commit_within_ms` | Publish within this many milliseconds. `0` is immediate; a positive value lets the update response return before publication. |
| `build_aux_indexes` | Missing vector overlays to build, such as `["*"]` or `["vec.embedding_v"]`. Existing overlays are retained; empty requests no builds. |
| `wait_for_merges` | Wait for in-flight merges before publishing. |
| `max_segments` | Force the committed data down to at most this many segments before returning. `0` means no forced merge. |

Commit without adding documents by sending `{"commit":{}}`. On the NDJSON
HTTP path, `?commit=true` is also available as a convenient end-of-stream
commit. Frequent forced merges are expensive; `max_segments` is an explicit
maintenance action, not a normal ingest setting.

Each update message uses the schema pinned at admission. After a successful
schema publication, newly admitted messages use its definitions; already
admitted work keeps its original schema. A long NDJSON stream can span several
messages and schema generations.

Adding a variant does not backfill earlier documents, and merging does not
create missing representations. Reindex the producer's input to populate them.
The [resolved schema view](schema.md#resolved-view) reports conservative
coverage; [live schema edits](schema.md#changes-on-a-live-collection) reject
incompatible reuse of a physical name with materialized data.

## Per-document failures

Unless `all_or_none` is set, document validation failures do not poison their
neighbors. A failed document has no effect and an older version with the same
ID remains intact:

```json
{
  "update_version": 9,
  "status": "partial",
  "errors": [
    {"id": "b2", "index": 1, "error": {"kind": "invalid_request", "code": "unknown_field", "message": "..."}}
  ],
  "total_errors": 1
}
```

Each entry names the document by `id` and by `index` in the request and
carries the same `error` object every Luxir error uses: `kind`, a stable
`code` (`invalid_value` for a value the field rejects, `unknown_field` for a
field the schema does not define), and a human `message`. A document that an
engine fault stopped is reported the same way with kind `internal`.
`total_errors` counts every failed document; it exceeds the length of
`errors` only when a transport retained a prefix of them.

A value rejected by any variant fails the entire document. The message names
the logical field, branch label (`self` for the primary), and cause. For example,
if the author example's `s` variant sets `long_terms: "reject"`, a 256-byte
normalized string fails branch `s` even if the TEXT primary accepted it.
By default, indexed STRING values truncate after normalization and TEXT tokens
truncate after analysis to at most 255 UTF-8-safe bytes. The stored source
keeps the full value. Column-only strings have no term-space limit; IDs always
truncate. See [term-space limits](documents.md#ids-and-replacement) for prefix
collisions and the opt-in reject policy.

The response status is:

- `ok`: every attempted change succeeded.
- `partial`: some documents succeeded and some failed.
- `error`: no document took effect, or a request-level failure occurred. A
  request-level failure (a bad `field_map`, the commit pipeline, a closed
  writer) also sets the top-level `error` object; per-document failures leave
  it unset.

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

{"id":"b1","title_t":"Dune","year_i":1965}
{"id":"b2","title_t":"Dune Messiah","year_i":1969}
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
`"collection":"archive"` for the following group, which lets one
connection feed several collections. Options do not leak from one group into
the next.

For example, two independently reported groups followed by one commit:

```jsonl
{"_update_":{"request_id":"fiction","return_ids":true}}
{"id":"b1","kind_s":"fiction","title_t":"Dune"}
{}
{"_update_":{"request_id":"nonfiction"}}
{"id":"b2","kind_s":"nonfiction","title_t":"The Making of the Atomic Bomb"}
{"_end_":{"commit":{}}}
```

The response is NDJSON too, one update response per completed group. A stream
may retain only the first 100 returned IDs and errors for a group; indexing
and `total_errors` are not capped by that reporting bound. A request-level
failure ends the stream: its response line carries `status: "error"` and the
`error` object alongside whatever the group had already reported, and no later
records from that connection are accepted.

To send an existing file without letting `curl` reinterpret newlines:

```bash
curl -X POST http://localhost:9400/collections/books/_update \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary @books.ndjson
```

The document-per-line [search export](searching.md#stream-every-match) uses this
input framing. Its `_header_` records are recognized and skipped. Export logical
field names for ingestion: explicit variant selectors are not document keys.
Retrieved primaries may have lost lexical input needed by variants, so a pipe
is not a general replacement for reindexing from the producer's source.

## Atomic streams are deliberately bounded

An unbounded stream and all-or-none atomicity cannot both be promised: rollback
requires retaining the atomic unit. An NDJSON group with `all_or_none: true` is
therefore accumulated as one bounded request and is subject to
`--indexing.max-request-body`. Ordinary streaming groups remain unbounded.

The relevant server controls are:

- `--indexing.max-request-body` for buffered JSON bodies and atomic groups
  (default `32MB`).
- `--indexing.max-record` for one NDJSON record (defaults to the buffered-body
  limit).
- `--indexing.stream-batch-size` and `--indexing.stream-batch-docs` for internal
  non-atomic handoff granularity.

At present a single HTTP NDJSON connection pipelines storage and indexing but
does not fan several internal batches into the engine concurrently. Use
several input streams when maximum ingest throughput matters. This is a
throughput detail, not a size limit or a client-visible batch contract.
