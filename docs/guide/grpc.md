# gRPC API

HTTP is the fastest way to explore Solux; gRPC is the typed, binary surface for
applications that want long-lived bidirectional streams and columnar search
results. Both reach the same request and execution model. The protobuf files
are the source of truth:

- [`protos/solux.proto`](../../protos/solux.proto) defines services.
- [`protos/solux_types.proto`](../../protos/solux_types.proto) defines search,
  update, schema, document, facet, and response messages.

The default gRPC port is one greater than the HTTP port: `9401` when HTTP uses
`9400`. Override it with `--server.grpc.port`.

## Services

| Service and method | Shape | Purpose |
|---|---|---|
| `solux.Indexer/Update` | unary | One bounded update request and response. |
| `solux.Indexer/UpdateStream` | bidirectional stream | A stream of independently acknowledged update requests. |
| `solux.Searcher/Search` | bidirectional stream | Several search requests per call, with one or more response batches per request. |
| `solux.Admin/SetSchema` | unary | Set named definitions or replace a collection schema. |
| `solux.Admin/GetSchema` | unary | Read a collection schema. |
| `solux.Admin/Stats` | unary | Read node-wide or collection index statistics. |

The server also registers the standard gRPC health service and descriptor
reflection. Solux implements the application services through gRPC's generic
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
  protos/solux_types.proto protos/solux.proto
```

Use the equivalent `grpc_tools.protoc`, Gradle, Go, Rust, or other language
plugin for your client. Solux does not yet publish packaged generated clients.

Reflection can inspect a known service without local proto files:

```bash
grpcurl -plaintext localhost:9401 describe solux.Searcher
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
- `freshness_us` and `time_zone`: request-level view/date controls.
- `profile`: include instrumented per-segment execution details on the final
  response; string field facets are currently instrumented.
- `max_parallel`: `0` for automatic intra-request parallelism, `1` for a
  single worker thread, or `-1` to run the whole request inline on the
  transport thread that received it (no scheduler handoff; blocks that thread
  for the request's duration - a diagnosis/measurement mode). Larger values
  are reserved and rejected.

Unlike HTTP, gRPC does not have the root `top_docs` shorthand. Populate the
`ops` map explicitly. It also does not use HTTP's document-line
`response_format=docs`; gRPC messages are already framed, and setting that
format is rejected.

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
completion timing. Responses may complete out of request order. The current
server ignores `stream_id`; it does not create a serialized substream.

Send documents through the repeated `docs` row maps. Although `UpdateRequest`
already reserves a `columns` map for a future aligned-column representation,
the current handler returns without indexing when it is non-empty and does not
yet report that as an error. Do not populate `columns`. Row maps use the field
coercion, update, and commit semantics described in [Indexing](indexing.md).

## HTTP JSON versus protobuf values

The HTTP surface is a deliberate JSON dialect over the protobuf model: a
`Val` is an untagged JSON value, match queries accept field-name sugar, a bare
query string means `expr`, and a root object can stand for one `top_docs`
operation. Generated gRPC clients use the protobuf messages directly, so they
set the relevant oneof fields and maps rather than those JSON conveniences.

The semantics still line up. `?explain=request` on HTTP is useful for seeing
which canonical request message a shorthand becomes before translating that
shape into generated-client calls.

## Transport and deployment boundary

The configured gRPC port listens on all interfaces with insecure server
credentials. There is no built-in TLS, authentication, authorization, or
request tenancy layer. Put it behind the same private network, proxy, or
service-mesh security boundary as the HTTP port; see
[Operating Solux](operations.md).
