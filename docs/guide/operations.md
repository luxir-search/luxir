# Operating Solux

Solux currently has a deliberately narrow deployment model: one process uses
one machine well and can host many isolated collections. Scale the machine for
capacity. Replication, sharding across nodes, failover orchestration, snapshots,
and a collection-management API are not built into the server yet.

That boundary matters more than a long tuning checklist. The defaults are good
for evaluation; persistence and network isolation are the two settings to make
explicit before keeping real data.

## Persistent storage

The default backend is in-memory and loses every collection when the process
exits:

```bash
solux
```

Use the filesystem backend for durable data:

```bash
solux --store.backend=fs --store.data-dir=/srv/solux/data
```

Each collection is an independent index below the data directory. Collections
present there are discovered at startup; a write to a new collection creates
its storage by default.

Segments are immutable and read with `mmap`. A commit writes and syncs new
files, publishes the metadata commit point with an atomic rename, then makes
the view available to readers. A crash during publication therefore leaves the
previous commit point, not a half-named view. Updates accepted after the last
published commit can be lost on process or machine failure.

Use local storage with reliable `fsync` and atomic-rename behavior. The optional
checked-directory mode diagnoses filesystems that violate the sync assumptions:

```bash
solux --store.backend=fs --store.data-dir=/srv/solux/data \
      --store.checked-dir.sync=warn
```

Valid modes are `off`, `warn`, and `throw`. `throw` turns a failed durability
check into an operation failure; use it deliberately rather than discovering a
filesystem incompatibility under load.

There is no online snapshot API. For a conservative current backup procedure,
stop writes, publish a commit, stop the process, and copy the data directory as
a unit. Do not infer a supported live-backup protocol merely from immutable
segment files: the metadata and files still need one consistent capture point.

## Visibility policy

Commits are the freshness boundary. An immediate `"commit": {}` waits until a
new index view is published. `commit_within_us` can coalesce publication and
return before the update is visible while still bounding staleness.

Search `freshness_us` expresses how stale a reader may be; `0` requires the
latest commit. Decide these together:

- Interactive writes that must be read immediately should use an immediate
  commit.
- Sustained feeds should use a positive commit interval rather than publishing
  a new view for every small update.
- Exact publication latency is a workload choice; forced merging is not.

`max_segments` and `wait_for_merges` are explicit maintenance controls. A
normal commit lets background merging choose the layout while ingest and search
continue.

## Network ports and security

Defaults:

| Surface | Port | Control |
|---|---:|---|
| HTTP/JSON | 9400 | `--server.http.port` or `-p` |
| gRPC | 9401 | `--server.grpc.port` (defaults to HTTP port plus one) |

A configured nonzero port binds `0.0.0.0`; there is no listen-address option
yet. Disable HTTP with `--no-http`. The gRPC server always starts.

Solux does **not** provide TLS, authentication, authorization, per-tenant
quotas, or a permission model. Do not expose either port directly to an
untrusted network. Bindings currently make network policy mandatory: place the
process in a private network namespace, firewall both ports, or front them with
an authenticated TLS proxy/service mesh.

The public parser is still treated as hostile input: unknown JSON keys are
errors, request/record sizes are bounded, parser nesting is limited, and
streaming output is backpressured. Those are robustness properties, not a
substitute for an identity and authorization layer.

## Health and logging

HTTP liveness:

```bash
curl http://localhost:9400/health
```

```json
{"status":"ok"}
```

gRPC registers the standard health service. The current HTTP check is process
liveness, not a deep read/write check of every collection or storage device.

Set log verbosity with:

```bash
solux --log-level=info
```

Valid spdlog levels include `trace`, `debug`, `info`, `warn`, `error`, and
`critical`. Startup logs include the loaded time-zone database version.

## Threads and streaming backpressure

HTTP and gRPC each default to half the detected hardware threads, with a
minimum of one:

```bash
solux --server.http.threads=8 --server.grpc.threads=8
```

These are network event-loop/completion-queue threads, not the search worker
pool. Search work runs on a shared work-stealing scheduler so long queries do
not occupy connection threads. The current gRPC unary update handler waits for
indexing on its completion-queue thread; streaming update and search use their
separate flow-control paths.

Search requests normally use automatic intra-request parallelism. Set the
request-level `max_parallel` to `1` for single-threaded diagnosis or controlled
profiling; `0` is automatic and values above `1` are not implemented yet.

`--server.stream_buffer_bytes` sets the per-HTTP-connection and per-gRPC-call
high-water mark for serialized responses (default 1 MiB). Producers pause above
it and resume after the buffer drains below half. Increase it only when more
per-connection buffering is a measured win; total memory scales with the number
of simultaneously slow streams.

## Indexing memory and ingest limits

```bash
solux \
  --index.max-inverter-ram-mb=64 \
  --index.max-inverter-docs=8388608 \
  --max-index-ram=8192
```

- `index.max-inverter-ram-mb` and `index.max-inverter-docs` trigger automatic
  segment flushes that bound active indexing structures.
- `max-index-ram` is the shared node-wide MiB budget used for merge admission;
  `0` leaves it unlimited.

The HTTP ingest limits distinguish bounded material from streams:

```bash
solux \
  --ingest.max-request-body=32MB \
  --ingest.max-record=32MB \
  --ingest.stream-batch-size=1MB \
  --ingest.stream-batch-docs=10000
```

- `max-request-body` caps buffered JSON requests and atomic NDJSON groups.
- `max-record` caps one streamed NDJSON record so a missing newline cannot grow
  memory without bound. It inherits `max-request-body` when unset.
- `stream-batch-size` and `stream-batch-docs` are internal handoff thresholds,
  not limits on the number of documents in one HTTP stream.

A single NDJSON connection does not yet fan several internal batches into the
index concurrently. Several producer streams are currently required to drive
maximum ingest throughput on a many-core machine.

## Time-zone data

IANA time-zone names require the system zoneinfo database and compatible C++
runtime data. Without it, date queries retain UTC and fixed-offset support but
named zones are unavailable. Keep the zoneinfo data and C++ runtime consistent
across machines if requests must produce identical civil-time boundaries.

## Shutdown and recovery

Engine/server components have drainable shutdown paths, but the current
`solux` executable does not install an application signal handler to invoke
them. A normal `SIGTERM`/`SIGINT` therefore terminates the process rather than
waiting for in-flight requests. Before a planned stop, quiesce producers and
publish an immediate commit. After an unplanned stop, the filesystem backend
reopens the last durable commit.

At startup, a collection whose top-level index metadata cannot be parsed is
kept as a tombstone rather than preventing healthy collections from loading.
Queries and updates to that collection return the recorded load error and the
server will not silently recreate storage over it. Other segment corruption is
detected when the affected reader is opened.

## Current production boundary

Before treating Solux as a production service, account explicitly for the
features it does not yet supply:

- single node, with no replication or distributed query execution;
- no built-in TLS/authentication/authorization;
- no online snapshot, restore, list, create, or delete collection APIs;
- no application-level signal-driven graceful shutdown;
- pre-1.0 wire and schema interfaces that may change.

These are product boundaries, not hidden deployment modes. The engine already
has the pieces that should remain stable as those layers arrive: immutable
commits, isolated collections, transport-independent request messages, and a
node-wide scheduler/memory model.
