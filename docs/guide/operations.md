# Operating Luxir

Luxir currently has a deliberately narrow deployment model: one process uses
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
luxir
```

Use the filesystem backend for durable data:

```bash
luxir --store.backend=fs --store.data-dir=/srv/luxir/data
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
luxir --store.backend=fs --store.data-dir=/srv/luxir/data \
      --store.checked-dir.sync=warn
```

Valid modes are `off`, `warn`, and `throw`. `throw` turns a failed durability
check into an operation failure; use it deliberately rather than discovering a
filesystem incompatibility under load.

There is no online snapshot API. For a conservative current backup procedure,
stop writes, publish a commit, stop the process, and copy the data directory as
a unit. Do not infer a supported live-backup protocol merely from immutable
segment files: the metadata and files still need one consistent capture point.

## Read-only nodes

A data directory has exactly one writer. The owning process takes `write.lock`
under the data directory at startup and holds it until exit, so a second writer
against the same directory fails to start rather than corrupting the index.

`--read-only` opens an existing data directory without that lock:

```bash
luxir --read-only --store.backend=fs --store.data-dir=/srv/luxir/data
```

A read-only node writes nothing at all - no lock file, no trash directory, not
even the data directory itself, which must already exist. It serves searches,
schema reads, and `_stats`. Every mutation is refused: updates, NDJSON streams,
schema writes, and collection create/delete return HTTP `403` (gRPC
`FAILED_PRECONDITION`), and collections are never auto-created. The refusal is
enforced twice - once at request dispatch for a clean error, and again at the
storage layer, which rejects any write regardless of the path that reached it.

Use it to query a directory another instance is writing, or to inspect one
offline without risking a stray write. Three current limitations matter:

- **The view does not advance.** A read-only node pins its index view the first
  time it serves a query and never reopens, so commits the writer publishes
  after that point are invisible until the read-only node restarts.
- **`_stats` reports a different point in time** than searches do: it reflects
  the directory as it was read at startup, while searches reflect the commit
  pinned at the first query.
- **Pinned files are not reclaimed.** Segment files are held open by `mmap`, so
  files the writer deletes stay on disk until the read-only node exits. A
  long-lived read-only node against a busy writer holds disk space that `du`
  attributes to no visible file.

Restart the read-only node to pick up newer commits and release pinned files.

## Collection lifecycle

Create and delete collections with `POST /collections/_create` and
`POST /collections/_delete` (or unary `luxir.Admin/CreateCollection` /
`DeleteCollection`). Deleting a collection also deletes its stored data; there
is no undo. `GET /collections` (canonically `/collections/_list`) returns the
sorted collection names, including any that failed to load; `/_stats` has the
per-collection detail.

Deletion is synchronous and wins over concurrent use. When the call returns,
the name resolves to nothing, the on-disk data is gone, and the name can be
recreated as a fresh empty collection. Requests racing the deletion fail
cleanly per request: an update batch or NDJSON stream that arrives after
deletion starts receives an error response, as does a search that resolves the
collection after that point. A search already executing is unaffected - it
holds its index view for the whole request and completes with correct results.
Deletion waits for indexing work already accepted, including a running merge,
so deleting a collection mid-merge can take as long as that merge.

On the filesystem backend a deleted collection is first renamed into `trash/`
under the data directory and then removed; `trash/` is purged again at startup,
so a crash mid-deletion cannot resurrect a partially deleted collection. If
deletion fails partway (for example an I/O error), the name stays unavailable
with the recorded error and the delete can simply be retried. Deleting a
collection that failed to load at startup is also the supported way to clear
its broken on-disk state.

## Visibility policy

Commits are the freshness boundary. An immediate `"commit": {}` waits until a
new index view is published. `commit_within_ms` can coalesce publication and
return before the update is visible while still bounding staleness.

Search `freshness_ms` expresses how stale a reader may be; `0` requires the
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

Luxir does **not** provide TLS, authentication, authorization, per-tenant
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

Use `GET /_stats` for node-wide operational state or
`GET /collections/{collection}/_stats` for one collection. The response
distinguishes writer-visible segments from committed segments and reports
document counts, generations, merge activity, filter-cache counters, and the
node-wide indexing RAM budget. Per-segment records are omitted by default; add
`?segments=true` when diagnosing segment layout. A node-wide sample includes
load-failure tombstones with their `error` and excludes them from totals. Each
collection index is sampled coherently, but a node-wide response is not one
transaction across collections. The same payload is available through unary
`luxir.Admin/Stats`; omit its collection target for the node-wide view.

Counts follow the JSON dialect: a zero-valued field is omitted rather than
emitted, so a scraper must read an absent field as zero. Totals appear at the
node, collection, and index levels, and each level sets only the counts that
mean something there - `collections` only on the node total, `shards` only on
node and collection totals.

Totals also report on-disk `bytes`, where the index level counts the whole
directory (manifest, schema files, in-flight files), so it can exceed the sum
of segment bytes. With `?segments=true` each segment reports its own `bytes`
(data + deletes + overlays) and each aux entry reports the bytes of its listed
files.

Ids and generations that appear in filenames use their filesystem spelling so
the response correlates directly with a directory listing: each segment's
`seg` is its data-file prefix (e.g. `s0a`), while `live_gen`, `schema_gen`,
and aux `gen` are the sortable strings embedded in filenames (segment `s0a`
with `live_gen` `01` has its deletes in `s0a__L01`; `schema_gen` `02` is the
file `_schema_02`). These strings sort in generation order, and an absent
field means none (no deletes file, no schema). Generations that never appear
on disk (`index_gen`, `core_gen`, `update_version`) stay numeric.

Set log verbosity with:

```bash
luxir --log-level=info
```

Valid spdlog levels include `trace`, `debug`, `info`, `warn`, `error`, and
`critical`. Startup logs include the loaded time-zone database version.

## Threads and streaming backpressure

HTTP defaults to one connection-I/O shard per detected hardware thread, with a
minimum of one, plus one dedicated accept thread. gRPC defaults to half the
detected hardware threads, also with a minimum of one:

```bash
luxir --server.http.threads=8 --server.grpc.threads=8
```

`--server.http.threads` sets the number of shards, not the total HTTP thread
count; each shard owns one `io_context` and one runner. Connections are
assigned round-robin at accept and remain pinned to their shard. These and the
Search requests default to serial execution directly on the transport thread
that received them (an HTTP io shard or gRPC completion-queue thread): no
scheduler handoff, no idle-worker wakeups, at the cost of occupying that
connection's thread for the query's duration. The request-level `max_parallel`
moves a request onto the shared work-stealing scheduler instead: `1` runs it
serially there (use this for requests expected to be expensive, so they do not
delay other traffic on the same connection thread), `-1` runs it with
unlimited intra-request parallelism, and values above `1` are reserved for a
bounded parallelism budget and not implemented yet. The current gRPC unary
update handler waits for indexing on its completion-queue thread; streaming
update and search use their separate flow-control paths.

`--server.stream_buffer_bytes` sets the per-HTTP-connection and per-gRPC-call
high-water mark for serialized responses (default 1 MiB). Producers pause above
it and resume after the buffer drains below half. Increase it only when more
per-connection buffering is a measured win; total memory scales with the number
of simultaneously slow streams.

## Memory budgets

`--max-ram-mb` is the node-wide budget (MiB) for memory Luxir manages
explicitly. It defaults to 25% of system RAM - or of the cgroup memory limit
when the process runs under one, so a container is sized by what the kernel
actually enforces rather than by the host's RAM. Subsystem budgets are derived
from it, so raising or lowering this one value moves them together. `0` leaves
the node unlimited. The resolved values are logged at startup.

```bash
luxir --max-ram-mb=16384          # node budget; indexing gets 8192
luxir --indexing.max-ram-mb=4096  # override just the indexing share
```

## Indexing memory and request limits

```bash
luxir \
  --indexing.max-ram-mb=8192 \
  --indexing.max-inverter-ram-mb=2048
```

- `indexing.max-ram-mb` is the shared MiB budget for all of a node's indexing
  work: merges reserve against it before they start, and inverters count their
  RAM against it, so a release that finds the budget over its cap flushes the
  largest idle inverter. It defaults to half of `--max-ram-mb`, and to none at
  all on a `--read-only` node, which never indexes. `0` leaves it unlimited.
- `indexing.max-inverter-ram-mb` flushes one inverter to a segment when it grows
  past that size, checked at the end of an update batch. Total indexing RAM is
  `indexing.max-ram-mb`'s job, so this cap defaults to that shared cap: one
  inverter may hold the whole indexing budget (with a single stream there is
  nothing else holding it) but no more. It is clamped to 3814 MiB - an inverter's
  memory pool can address at most 4 GiB, and the clamp leaves headroom for one
  batch's overshoot - so an explicit larger value is clamped with a startup
  warning.
- Lower `indexing.max-inverter-ram-mb` to trade merge work for flush latency and
  peak RSS: smaller flushes land sooner and cost less at once, at the price of
  more segments to merge.

The HTTP request limits distinguish bounded material from streams:

```bash
luxir \
  --indexing.max-request-body=32MB \
  --indexing.max-record=32MB \
  --indexing.stream-batch-size=1MB \
  --indexing.stream-batch-docs=10000
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
`luxir` executable does not install an application signal handler to invoke
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

Before treating Luxir as a production service, account explicitly for the
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
