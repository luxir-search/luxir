# gRPC API

HTTP is the fastest way to explore Luxir; gRPC is the typed, binary surface for
applications that want long-lived bidirectional streams and columnar search
results. Both reach the same request and execution model. The protobuf files
are the source of truth:

- [`protos/luxir.proto`](../../protos/luxir.proto) defines services.
- [`protos/luxir_types.proto`](../../protos/luxir_types.proto) defines search,
  update, schema, document, facet, and response messages.

Both files use the `luxir` package, so services and their request and response
types share one namespace (for example, `luxir.SearchRequest`).

For how `SearchRequest`, `SearchOp`, and `Query` fit together, see
[Queries and search operations](searching.md#queries-and-search-operations).

The default gRPC port is one greater than the HTTP port: `9401` when HTTP uses
`9400`. Override it with `--server.grpc.port`.

## Services

| Service and method | Shape | Purpose |
|---|---|---|
| `luxir.Indexer/Update` | unary | One bounded update request and response. |
| `luxir.Indexer/UpdateStream` | bidirectional stream | A stream of independently acknowledged update requests. |
| `luxir.Searcher/Search` | bidirectional stream | Several search requests per call, with one or more response batches per request. |
| `luxir.Admin/SetSchema` | unary | Set named definitions or replace a collection schema. |
| `luxir.Admin/GetSchema` | unary | Read a collection schema. |
| `luxir.Admin/CreateCollection` | unary | Create a collection, optionally with a schema. |
| `luxir.Admin/DeleteCollection` | unary | Delete a collection and its stored data. |
| `luxir.Admin/Stats` | unary | Read node-wide or collection index statistics. |

The server also registers the standard gRPC health service and descriptor
reflection. Luxir implements the application services through gRPC's generic
byte transport, so the stock reflection plugin can resolve known symbols but
does not currently enumerate those services in `ListServices`. Ask for a known
symbol directly when using a reflection client.

## Generate a client

Compile both protobuf files with the standard gRPC generator for your language.
For C++ the shape is:

```bash
mkdir -p generated
protoc -I protos \
  --cpp_out=generated \
  --grpc_out=generated \
  --plugin=protoc-gen-grpc="$(command -v grpc_cpp_plugin)" \
  protos/luxir_types.proto protos/luxir.proto
```

Use the equivalent `grpc_tools.protoc`, Gradle, Go, Rust, or other language
plugin for your client. Luxir does not yet publish packaged generated clients.

Reflection can inspect a known service without local proto files:

```bash
grpcurl -plaintext localhost:9401 describe luxir.Searcher
```

## Search streams

`Searcher.Search` is bidirectional. A client can keep one call open and send
several `SearchRequest` messages. Each request may produce several
`SearchResponse` messages when a document list is batched; `more` tells the
client another response belongs to the same logical request. Set `request_id`
when several requests share a call and use the echoed value for correlation.

One request contains:

- `collection`: the target collection.
- `ops`: named `top_docs`, `fusion`, facet, range-facet, or metric operations.
- `freshness_ms` and `time_zone`: request-level view/date controls.
- `profile`: include instrumented per-segment execution details on the final
  response; string field facets are currently instrumented.
- `max_parallel`: maximum intra-request parallelism on the shared worker
  pool - `1` for serial, `-1` for unlimited. `0` (the default) lets the
  engine decide; today that runs the request serially, inline on the
  completion-queue thread that received it (no scheduler handoff; occupies
  that thread for the request's duration). Larger values are reserved.

Unlike HTTP, gRPC does not have the root `top_docs` shorthand. Populate the
`ops` map explicitly. It also does not use HTTP's document-line
`response_format=docs`; gRPC messages are already framed.

Returned documents default to `DocFormat.COLUMNS`. `DocList.row_count` is the
authoritative number of rows. Each requested dense column has that many slots
and its type-specific missing sentinel identifies absent values. `DocList.docs`
contains per-row maps when row placement is requested or when a field is not
placed in a dense column. A document row is the merge of `columns[i]` and
`docs[i]`.

## Update streams

`Indexer.UpdateStream` accepts a series of normal `UpdateRequest` messages and
returns one `UpdateResponse` for each. Each message has its own overwrite,
atomicity, return-ID, and commit settings; all-or-none never spans messages.
`request_id` is echoed so responses can be associated without depending on
completion timing. Responses may complete out of request order.

Send documents through the repeated `docs` row maps. Row maps use the field
coercion, update, and commit semantics described in [Indexing](indexing.md).

## Errors

A unary method fails with a gRPC status whose code follows the error's
`kind`: `INVALID_ARGUMENT`, `NOT_FOUND`, `ALREADY_EXISTS`,
`FAILED_PRECONDITION`, `RESOURCE_EXHAUSTED`, `UNAVAILABLE`, or `INTERNAL`.
The status message is the human detail, and the status details
(`grpc-status-details-bin`) carry a `google.rpc.Status` whose single detail is
the `luxir.Error` message, so a generated client reads the stable `code`
and `kind` without parsing text.

Streaming methods report a failure of an accepted request in-band with the
same `Error` message: `SearchResponse.error` (the response then carries no
`ops`, declared `warnings` still ride along, and every earlier batch of that
request is invalidated) and `UpdateResponse.error` for a request-level update
failure, with per-document failures in `UpdateResponse.errors[].error` and
`total_errors` counting them. `request_id` is echoed on both. A message that
cannot be accepted at all ends the call with a status instead: one that does
not decode, a search asking for the HTTP-only docs format, or an update whose
collection cannot be resolved. Responses already accepted are still delivered
before that status. The kinds, codes, and their meanings are
transport-independent; see [HTTP API conventions](http-api.md#errors) for the
table.

## HTTP JSON versus protobuf values

The HTTP JSON accepts shorthands that the protobuf messages do not: a
`Val` is an untagged JSON value, match queries accept field-name sugar, a bare
query string means `expr`, and a root object can stand for one `top_docs`
operation. Generated gRPC clients use the protobuf messages directly, so they
set the relevant oneof fields and maps rather than those JSON conveniences.

`?explain=request` on HTTP shows the canonical request message a shorthand
becomes, which is the message a generated client should build.

## Transport and deployment boundary

The configured gRPC port listens on all interfaces with insecure server
credentials. There is no built-in TLS, authentication, authorization, or
request tenancy layer. Put it behind the same private network, proxy, or
service-mesh security boundary as the HTTP port; see
[Operating Luxir](operations.md).
