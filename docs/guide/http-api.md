# HTTP API conventions

The HTTP surface is plain JSON for bounded messages and NDJSON for streams. It
is deliberately small: collection names live in the path and the request body
uses the same vocabulary as protobuf.

## Endpoints

| Method and path | Purpose |
|---|---|
| `GET /health` | Process liveness. |
| `GET` or `POST /collections/{collection}/_search` | Search; URL request fields and optional POST JSON body, chunked NDJSON response. |
| `POST /collections/{collection}/_update` | Bounded JSON update or unbounded NDJSON ingest. |
| `GET /collections/{collection}/_schema` | Read the authored schema. |
| `GET /collections/{collection}/_schema?view=resolved` | Read physical representations, bindings, and conservative schema coverage; HTTP only. |
| `POST /collections/{collection}/_schema` | Set definitions or replace the schema. |
| `GET /collections/_list` (or `POST`) | List collection names; `GET /collections` is a synonym. |
| `POST /collections/_create` (or `PUT`) | Create a collection; body `{"name": "...", "schema": {...}}`. |
| `POST /collections/_delete` | Delete a collection and its stored data; body `{"name": "..."}`. |
| `GET /_stats` | Node totals and per-collection operational statistics. |
| `GET /collections/{collection}/_stats` | Operational statistics for one collection. |

Collection names occupy one URL path component. Names beginning with `_` are
reserved. Search and schema reads never create a missing collection; an update
does by default unless `--no-indexing.auto-create-collection` is set.

`_create` is strict: creating a name that already exists is a `409` error, and
the optional `schema` (same shape as a `_schema` set) is installed before the
collection becomes visible, so no request can observe the collection with only
the default schema. `_delete` names its target in the body, never falls back to
a default collection, and returns `404` for a missing name. See the operations
guide for deletion semantics under concurrent use.

## Content types and framing

Send bounded search, schema, and update messages as `application/json`. Send a
document stream to `_update` as `application/x-ndjson`; one complete JSON value
must end on each line.

GET search requests have no body. Search request fields may be written as typed
URL parameters; on POST they overlay the parsed JSON body, with URL values
winning field by field. See [Searching](searching.md#url-request-field-overlay)
for the whitelist and value grammars.

Search responses use `application/x-ndjson` with HTTP chunked transfer coding.
Each normal line is one complete response batch. A small default-limit search
usually has one line and therefore also parses as an ordinary JSON object. Use
`?format=docs` for document-per-line export without response envelopes.

Every JSON body ends with a newline. `?pretty` (bare, or `?pretty=true` /
`?pretty=false`) is a URL-only, best-effort formatting hint that applies to
every JSON body of the request, errors and ingest acknowledgements included.
Schema, collection list, and stats responses default to pretty; every other
route defaults to compact. Indentation is two spaces but is not part of the
contract. A pretty stream is sent as `application/json`, still chunked, with
one pretty JSON text per batch separated by blank lines: it is for eyes, not
for parsers expecting a single document, though `jq` reads the sequence
directly. Pretty is ignored for `format=docs`, whether selected by URL or
body, which stays NDJSON.

The server applies backpressure to streaming producers. It does not buffer an
unbounded request or response into one in-memory JSON value.

## JSON dialect

JSON field names and enums are lowercase `snake_case`. Values use natural JSON
forms where their message type is known. For example, a kNN query vector is an
array of numbers, a query can be a bare expression string, and
`{"match":{"title_t":"dune"}}` is accepted field-name sugar. Document vector
fields also accept bare number arrays. The schema interprets a number array as
one vector and an array of number arrays as a multi-valued vector list.

Unknown JSON keys, unknown oneof arms, invalid enum names, excessive nesting,
and wrong value shapes are request errors. A body typo is not ignored. URL
query parameters are intentionally an open middleware channel: unknown
parameters are currently accepted and ignored, while recognized parameters
validate their lexical values (including `pretty`). Add `?explain=request` to
a query to return the canonical effective request, including URL overlays,
without executing it; posting the result back has the same semantics.

The HTTP path collection is authoritative. Canonical echo may show it as
`"collection":"books"` even when the original body omitted it.

### Explain modes

`?explain=request` is parse-and-serialize only. It returns the bare canonical
request, including URL overlays, without acquiring readers or running semantic
preparation. The entire response can be posted back as a request.

`?explain=resolved` returns the request together with field-binding notes.
Using the `names` collection from [Schema](schema.md#field-variants):

```http
POST /collections/names/_search?explain=resolved

{"query":{"any_of":{"field":"author","values":["URSULA K. LE GUIN"]}},"fields":["id"]}
```

```json
{
  "request": {
    "collection": "names",
    "ops": {"q":{"top_docs":{
      "query":{"any_of":{"field":"author","values":["URSULA K. LE GUIN"]}},
      "fields":["id"]
    }}}
  },
  "resolved_fields": ["q: author -> author__s"]
}
```

Replay the `request` member. Its field spellings stay as authored;
`resolved_fields` contains contextual mappings to physical names where they
differ, with repeated identical notes removed. These strings are diagnostics,
not a serialized execution plan.

Resolved explain runs ordinary search preparation: it acquires readers,
respects freshness, validates semantics, and may do dictionary, weight, and
cache work. It does not execute result collection or calculators. It therefore
needs a usable collection and rejects errors such as a negative offset, which
request echo can return unchanged. Projection errors that arise during document
emission are not checked by this mode. Unknown explain modes are request errors.

## Errors

Every failure has one shape, wherever it appears:

```json
{"request_id": "q7", "error": {"kind": "invalid_request", "code": "unknown_field", "message": "Field not found: titel"}}
```

`kind` is the coarse class. It fixes the HTTP status and tells a client
whether to fix the request, the target, or retry, even for a `code` it has
never seen. `code` is the stable machine key; new codes appear under existing
kinds without changing the shape. `message` is human detail whose wording is
not part of the contract. `request_id` is echoed whenever the request carried
one, in the body or as a `?request_id=` URL parameter, and is omitted
otherwise.

| `kind` | HTTP status | Meaning |
|---|---|---|
| `invalid_request` | 400 (405 for `method_not_allowed`) | The request as written cannot be served. |
| `not_found` | 404 | The route or the collection does not exist. |
| `already_exists` | 409 | Creating a collection that already exists. |
| `failed_precondition` | 403 | The node's state forbids the operation, such as `--read-only`. |
| `resource_exhausted` | 429 (413 for `request_too_large`) | A size or memory ceiling was exceeded. |
| `unavailable` | 503 | The collection exists but cannot serve: it is being deleted or failed to load. |
| `internal` | 500 | A server-side failure the request did not cause. |

Codes today: `invalid_request`, `invalid_json`, `invalid_schema`,
`invalid_collection_name`, `invalid_field_name`, `invalid_field`, `invalid_value`, `unknown_field`,
`invalid_expression`, `method_not_allowed`, `request_too_large`,
`request_memory_exceeded`, `not_found`, `collection_not_found`,
`collection_unavailable`, `collection_exists`, `read_only`, `writer_closed`,
and `internal`. A failure that no more specific code describes carries its
kind's name as the code.

A failure detected before a request is submitted (malformed JSON, a bad URL
parameter, an unknown or invalid collection name, a wrong method) is answered
with the kind's HTTP status and this body. Method handling is uniform: every
known path answers a wrong method with `405` and an `Allow` header, and only
an unknown path is `404`.

Once a chunked response has begun, its HTTP status cannot change, so a search
that fails after submission arrives with HTTP 200 as the final NDJSON line in
the same shape, `{"request_id": ..., "error": {...}, "warnings": [...]}`,
with no `docs` or `ops`. That final error invalidates every earlier batch line
of the same request: a client that streamed `more: true` lines must discard
them. Document-only `format=docs` has no error envelope: a
failure before output becomes a normal error response, while a failure after
documents were emitted aborts the body without its terminating chunk so the
client sees truncation.

HTTP transport success and update success are separate. A syntactically valid
update returns HTTP success with `status: "partial"` and per-document
`errors`, each carrying the same `error` object; a request-level failure sets
`status: "error"` and a top-level `error`. Clients must inspect the body. NDJSON
ingest reports a request-level failure as a terminal response line after
earlier group acknowledgements; no later records from that connection are
accepted. See [Indexing](indexing.md#per-document-failures).

Search warnings are successful results with a bounded declared degradation.
Use `warnings[].code` as the machine key and the message as diagnostic detail;
warnings declared before a failure ride along with the error.

## Connection and security boundary

HTTP/1.1 keep-alive and request streaming are supported. A configured nonzero
HTTP port currently binds all interfaces, and the server has no TLS,
authentication, authorization, CORS policy, or tenant permission layer. Do not
expose it directly to an untrusted network. See
[Operating Luxir](operations.md#network-ports-and-security).
