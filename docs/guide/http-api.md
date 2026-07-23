# HTTP API conventions

The HTTP surface is plain JSON for bounded messages and NDJSON for streams. It
is deliberately small: collection names live in the path and the request body
uses the same vocabulary as protobuf.

## Endpoints

| Method and path | Purpose |
|---|---|
| `GET /health` | Process liveness. |
| `POST /collections/{collection}/_search` | Search; bounded JSON request, chunked NDJSON response. |
| `POST /collections/{collection}/_update` | Bounded JSON update or unbounded NDJSON ingest. |
| `GET /collections/{collection}/_schema` | Read the authored schema. |
| `POST /collections/{collection}/_schema` | Set definitions or replace the schema. |

Collection names occupy one URL path component. Names beginning with `_` are
reserved. Search and schema reads never create a missing collection; an update
does by default unless `--no-ingest.auto-create-collection` is set.

## Content types and framing

Send bounded search, schema, and update messages as `application/json`. Send a
document stream to `_update` as `application/x-ndjson`; one complete JSON value
must end on each line.

Search responses use `application/x-ndjson` with HTTP chunked transfer coding.
Each normal line is one complete response batch. A small default-limit search
usually has one line and therefore also parses as an ordinary JSON object. Use
`?format=docs` for document-per-line export without response envelopes.

The server applies backpressure to streaming producers. It does not buffer an
unbounded request or response into one in-memory JSON value.

## JSON dialect

JSON field names and enums are lowercase `snake_case`. Values use natural JSON
forms where their message type is known. For example, a kNN query vector is an
array of numbers, a query can be a bare expression string, and
`{"match":{"title_w":"dune"}}` is accepted field-name sugar. A vector inside
a dynamic document `Val` is the current exception: HTTP cannot select the
typed `vec` arm, so vector documents must be indexed through gRPC for now.

Unknown JSON keys, unknown oneof arms, invalid enum names, excessive nesting,
and wrong value shapes are request errors. A body typo is not ignored. URL
query parameters are intentionally an open middleware channel: unknown
parameters are currently accepted and ignored, while recognized parameters
validate their values. Add `?explain=request` to a query to return the canonical
parsed request without executing it; posting the result back has the same
semantics.

The HTTP path collection is authoritative. Canonical echo may show it as
`"collection":{"name":["books"]}` even when the original body omitted it.

## Success, partial updates, and errors

HTTP transport success and update success are separate. A syntactically valid
update can return HTTP success with `status: "partial"` and per-document
`errors`; clients must inspect the body. `all_or_none: true` changes that update
contract to rollback of the explicit atomic unit.

Malformed JSON and HTTP-dialect validation failures detected before submission
return `400` with a JSON error body. Oversized buffered bodies return `413`.
The schema routes return `405` with an `Allow` header for a wrong method; other
routes currently fall through to `404`, so method handling is not uniform.
After a normal search has been submitted, query planning or execution errors
appear as an `error` field in the HTTP-success NDJSON envelope. The server does
not yet have a complete HTTP status taxonomy, so clients must inspect response
bodies and must not key behavior to the human error-message text.

Once a chunked response has begun, its HTTP status cannot change. Normal
envelope mode reports a later search failure as a final `error` response and
terminates the stream cleanly. Document-only `format=docs` has no error
envelope: a failure before output becomes a normal error response, while a
failure after documents were emitted aborts the body without its terminating
chunk so the client sees truncation. NDJSON ingest can report a terminal error
record after earlier group acknowledgements; no later records from that
connection are accepted.

Search warnings are successful results with a bounded declared degradation.
Use `warnings[].code` as the machine key and the message as diagnostic detail.

## Connection and security boundary

HTTP/1.1 keep-alive and request streaming are supported. A configured nonzero
HTTP port currently binds all interfaces, and the server has no TLS,
authentication, authorization, CORS policy, or tenant permission layer. Do not
expose it directly to an untrusted network. See
[Operating Solux](operations.md#network-ports-and-security).
