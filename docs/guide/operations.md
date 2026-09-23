# Operating Luxir

Luxir runs as one process on one machine and hosts many isolated
collections. Capacity comes from the size of that machine.
Replication, sharding across nodes, failover orchestration, and snapshots are
not built into the server yet; the
[production boundary](#current-production-boundary) at the end of this page
lists what is missing.

The defaults are good for evaluation. Before you keep real data, decide on
persistence and network isolation. The other settings on this page can stay
at their defaults until you have a reason to change them.

## Persistent storage

The default backend is in-memory and loses every collection when the process
exits:

```bash
./luxir
```

Use the filesystem backend on local storage for durable data:

```bash
./luxir --store.backend=fs --store.data-dir=/srv/luxir/data
```

Each collection is an independent index below the data directory. Collections
present there are discovered at startup; a write to a new collection creates
its storage by default.

A commit makes updates durable and visible to searches. After a crash, Luxir
reopens the last durable commit. Updates accepted since that commit can be lost.

There is no online snapshot API. For a conservative current backup procedure,
stop writes, publish a commit, stop the process, and copy the entire data
directory. Copying the directory while Luxir is writing to it is not a safe
backup.

## Read-only nodes

A data directory has exactly one writer, which holds `write.lock` from startup
until exit.

`--read-only` opens an existing data directory without that lock:

```bash
./luxir --read-only --store.backend=fs --store.data-dir=/srv/luxir/data
```

A read-only node requires an existing data directory and writes no files.
It serves searches, schema reads, and `_stats`.

Use it to query a directory another instance is writing, or to inspect one
offline without risking a stray write. Current limitations:

- **The view does not advance.** A read-only node opens its index view at
  startup and never reopens, so later commits are invisible until it restarts.
- **`_stats` describes the same snapshot as searches.** Directory byte totals
  still reflect the files present when stats are requested.
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
recreated as a fresh empty collection. New requests receive
`collection_unavailable` while deletion is in progress. Searches already
executing keep their index view and complete normally.
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

Updates become visible to searches at commit. An
immediate `"commit": {}` waits until a
new index view is published. `commit_within_ms` can coalesce publication and
return before the update is visible while still bounding staleness.

Search `freshness_ms` expresses how stale a reader may be; `0` requires the
latest commit. Decide these together:

- Interactive writes that must be read immediately should use an immediate
  commit.
- Sustained feeds should use a positive commit interval rather than publishing
  a new view for every small update.
- Forced merges are a maintenance action, not part of a routine commit.

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
untrusted network. Network policy is mandatory: place the
process in a private network namespace, firewall both ports, or front them with
an authenticated TLS proxy/service mesh.

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
document counts, generations, merge activity, query-cache counters, and the
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
directory (manifest and in-flight files), so it can exceed the sum
of segment bytes. With `?segments=true` each segment reports its own `bytes`
(data + deletes + overlays) and each aux entry reports the bytes of its listed
files.

Ids and generations that appear in filenames use their filesystem spelling so
the response correlates directly with a directory listing: each segment's
`seg` is its data-file prefix (e.g. `s0a`), while `live_gen` and aux `gen`
are the sortable strings embedded in filenames (segment `s0a` with `live_gen`
`01` has its deletes in `s0a__L01`). These strings sort in generation order,
and an absent field means none. `schema_gen`, `index_gen`, `core_gen`, and
`update_version` are numeric. The schema and its history are embedded in each `s.olux_<index_gen>` manifest.

Filesystem publication syncs new data files and then the directory before
writing a new `s.olux_<index_gen>` manifest with a length and xxh3 checksum
footer. It syncs that manifest and then the directory before acknowledging
the commit. Startup checks the newest manifest's footer and decodes its payload;
it does not hash data files. Torn candidates are skipped. Fallback candidates
also require their referenced files to be present with matching sizes.

Once the new root is durable, obsolete manifests and unreferenced data are removed.
Transfer reservations are keyed by `(incarnation, index_gen)` and survive
individual requests. Clients downloading the same commit share one reservation;
release revokes that reservation for all of them. `snapshot_pins`, `pin_retained_bytes`, `pin_idle_drops`, and
`pin_budget_drops` report reservation state and policy drops. Retained bytes
count unique files whose only remaining owners are pins; current-snapshot
files and files still owned by indexing or merges do not count. The oldest
reservations are revoked first when the budget is exceeded. Snapshot acquisition renews the reservation; file transfers renew on byte
progress, at most once per second. A node-wide HTTP server timer checks expiry
every half idle-timeout on the task arena. Acquire, file open, retirement and
stats also check expiry; touch only updates the timestamp. Revocation aborts open
transfers, and each socket write has an idle deadline. Without an HTTP server,
internal reservations expire on activity. See [replication settings](replication.md).
Reservations are process-local; writer startup removes leftover unreferenced
index files using the directory listing, without reading their contents. If
startup falls back below the highest manifest generation, it logs an error and
skips this sweep, preserving newer files for recovery.

Removals are not directory-synced: a crash may restore obsolete names, but the
newest durable manifest does not reference them. Failed publication candidates
are removed and the directory is synced best-effort. An error response does not
guarantee that the commit is absent: a crash or cleanup failure can leave a
complete, unacknowledged candidate recoverable at startup. The local filename is not part
of the commit identity: clients use the `incarnation:index_gen` token.

Set log verbosity with:

```bash
./luxir --log-level=info
```

Valid spdlog levels include `trace`, `debug`, `info`, `warn`, `error`, and
`critical`. Startup logs include the loaded time-zone database version.

## Threads and streaming backpressure

HTTP defaults to one connection-I/O shard per available logical CPU, with a
minimum of one, plus one dedicated accept thread. gRPC defaults to half the
available logical CPUs, also with a minimum of one. Automatic sizing respects
the process's CPU affinity, so a server restricted to 28 logical CPUs defaults
to 28 HTTP shards and 14 gRPC threads. Set explicit counts with:

```bash
./luxir --server.http.threads=8 --server.grpc.threads=8
```

`--server.http.threads` sets the number of shards, not the total HTTP thread
count; each shard owns one `io_context` and one runner. Connections are
assigned round-robin at accept and remain pinned to their shard. These and the
Search requests default to serial execution directly on the transport thread
that received them (an HTTP io shard or gRPC completion-queue thread). This
avoids a scheduler handoff and idle-worker wakeups but occupies that
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
./luxir --max-ram-mb=16384          # node budget; indexing gets 8192
./luxir --indexing.max-ram-mb=4096  # override just the indexing share
```

## Indexing memory and request limits

```bash
./luxir \
  --indexing.max-ram-mb=8192 \
  --indexing.max-inverter-ram-mb=2048
```

- `indexing.max-ram-mb` is the shared MiB budget for all of a node's indexing
  work: merges reserve against it before they start, and inverters count their
  RAM against it, so a release that finds the budget over its cap flushes the
  largest idle inverter. It defaults to half of `--max-ram-mb`, and to none at
  all on a `--read-only` node, which never indexes. `0` leaves it unlimited.
- `indexing.max-inverter-ram-mb` flushes one inverter to a segment when it grows
  past that size, checked at the end of an update batch. Total indexing RAM
  is bounded by `indexing.max-ram-mb`, so this cap defaults to it:
  one
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
./luxir \
  --indexing.max-request-body=32MB \
  --indexing.max-record=32MB \
  --indexing.stream-batch-size=1MB \
  --indexing.stream-batch-docs=10000 \
  --indexing.max-inflight-batches=0
```

- `max-request-body` caps buffered JSON requests and atomic NDJSON groups.
- `max-record` caps one streamed NDJSON record so a missing newline cannot grow
  memory without bound. It inherits `max-request-body` when unset.
- `stream-batch-size` and `stream-batch-docs` are internal handoff thresholds,
  not limits on the number of documents in one HTTP stream.
- `max-inflight-batches` bounds the number of internal batches one connection
  can submit concurrently. `0` (the default) uses task-arena concurrency plus
  two; `1` makes batch submission serial. Reads pause when the limit is
  reached and resume as batches complete. A larger limit uses more staging
  memory.

Search has its own limits:

```bash
./luxir \
  --search.request-memory-max-bytes=0 \
  --search.max-op-depth=8 \
  --query-cache-bytes=64MB
```

- `search.request-memory-max-bytes` is a per-request breaker for query memory
  such as facet aggregate state; `0` (the default) is unlimited.
- `search.max-op-depth` bounds how deeply operations may nest in one request.
- `query-cache-bytes` caps each collection's filter cache (default `64MB`; `0`
  disables it). Cached filters are evicted by rebuild cost per byte, so under
  pressure the cache keeps expensive compound filters and drops cheap ones
  first. Cache counters appear in `_stats`.

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
Queries and updates to that collection return the recorded load error as an
`unavailable` error (HTTP `503`, code `collection_unavailable`) and the server
will not silently recreate storage over it. Other segment corruption is
detected when the affected reader is opened.

## Current production boundary

Before treating Luxir as a production service, account explicitly for the
features it does not yet supply:

- single node, with no replication or distributed query execution;
- no built-in TLS/authentication/authorization;
- no online snapshot or restore API;
- no application-level signal-driven graceful shutdown;
- pre-1.0 wire and schema interfaces that may change.

None of these can be enabled by configuration. The pieces those layers will
build on are already in place: immutable commits, isolated collections,
transport-independent request messages, and a node-wide scheduler and memory
model.

Startup orphan cleanup is skipped when root selection falls back from a torn
newest manifest. A later durable publication removes the invalid roots; the
next restart then reclaims remaining orphans. Cleanup is shared by snapshot
owners, so installers can use the same rules.
