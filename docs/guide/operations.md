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

Use [`luxir pull`](replication.md#copy-restore-or-promote) for an online copy,
then `luxir promote` to restore it as a writer under new collection incarnations.
Do not restore a raw directory copy over an existing writer identity.

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
  (`files=true`) still reflect the files present when stats are requested.
- **Pinned files are not reclaimed.** Segment files are held open by `mmap`, so
  files the writer deletes stay on disk until the read-only node exits. A
  long-lived read-only node against a busy writer holds disk space that `du`
  attributes to no visible file.

Restart the read-only node to pick up newer commits and release pinned files.
For a continuously refreshed copy with its own storage, use
[`--replicate-from http://writer:9400`](replication.md).
Followers own their local data directory and install verified snapshots without
constructing an index writer.

## Collection lifecycle

Create and delete collections with `POST /collections/_create` and
`POST /collections/_delete` (or unary `luxir.Admin/CreateCollection` /
`DeleteCollection`). Deleting a collection also deletes its stored data; there
is no undo. `GET /collections` (canonically `/collections/_list`) returns the
sorted collection names, including any that failed to load;
`/_stats?collections=true` has the per-collection detail.

Deletion is synchronous and wins over concurrent use. When the call returns,
the name resolves to nothing, the on-disk data is gone, and the name can be
recreated as a fresh empty collection. New requests receive
`collection_unavailable` while deletion is in progress. Searches already
executing keep their index view and complete normally.
Deletion waits for indexing work already accepted, including a running merge,
so deleting a collection mid-merge can take as long as that merge.

On the filesystem backend collections use
`t/<tenant>/<name>/<incarnation>/`, selected by an atomically replaced `CURRENT`
file in `t/<tenant>/<name>/`. Single-tenant deployments use the default tenant
`t0`, so the default collection lives in `t/t0/main/`. An incarnation is 16 hex
digits. `t/LAYOUT` names this layout; a data directory written in another layout
is refused at startup and must be reindexed. Deletion removes and
syncs `CURRENT` before removing the directory tree, so an interrupted deletion
cannot reopen the partially deleted index. If
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

Operational statistics and Prometheus metrics are described in
[Stats and monitoring](#stats-and-monitoring).

Filesystem publication syncs new data files and then the directory before
writing a new `commit_<index_gen>` manifest with a length and xxh3 checksum
footer. It syncs that manifest and then the directory before acknowledging
the commit. Startup checks the newest manifest's footer and decodes its payload;
by default it does not hash data files (see
[file integrity checks](#file-integrity-checks)). Torn candidates are skipped.
Fallback candidates also require their referenced files to be present with
matching sizes.

Once the new root is durable, obsolete manifests and unreferenced data are removed.
Replication pin counters are described in the
[replication reference](replication.md#reference). Replication file transfers have
a fixed 60 s idle deadline per socket write, reset as writes complete. Ordinary
HTTP responses have no write deadline.

Writer startup removes leftover unreferenced
index files using the directory listing, without reading their contents. If
startup falls back below the highest manifest generation, it logs an error and
skips this sweep, preserving newer files for recovery.

Removals are not directory-synced: a crash may restore obsolete names, but the
newest durable manifest does not reference them. Failed publication candidates
are removed and the directory is synced best-effort. An error response does not
guarantee that the commit is absent: a crash or cleanup failure can leave a
complete, unacknowledged candidate recoverable at startup. The local filename is not part
of the commit identity: clients use the `incarnation:index_gen` token. That
token's manifest is `<incarnation>/commit_<index_gen>`, with the generation in
decimal; tools that list manifests in order sort by the parsed generation.

Set log verbosity with:

```bash
./luxir --log-level=info
```

Valid spdlog levels include `trace`, `debug`, `info`, `warn`, `error`, and
`critical`. Startup logs include the loaded time-zone database version.

## Stats and monitoring

`_stats` returns one typed response, rendered as JSON by default or as
Prometheus text format 0.0.4 with `format=prometheus`. `GET /metrics` is the
node's Prometheus scrape: the same response and rendering, with
`collections=true` and `system=true` unless the URL says otherwise (it takes
the same parameters, so `format=json` returns that response as JSON). Both
renderings come from the same response. Prometheus carries every monitoring
value; generations, segment and aux index layout, and the pre-summed totals
are JSON-only (Prometheus sums the per-index series itself). A test fails when
a new field is in neither list. The same response is available through
unary `luxir.Admin/Stats` (structured protobuf; there is no text rendering on
gRPC). The endpoint shares the administration API's network access boundary
and includes host information and tenant/collection names.

| Route | Scope |
| --- | --- |
| `GET /_stats` | The node. |
| `GET /metrics` | The node, as Prometheus text with collections and system statistics. |
| `GET /tenants/{tenant}/_stats` | Every collection of one tenant. |
| `GET /collections/{collection}/_stats`, `GET /tenants/{tenant}/collections/{collection}/_stats` | One collection. |

The node scope returns the cheap counters by default; anything that walks
collections, lists directories, or reads the operating system is opt-in. A
tenant or collection scope always walks its collections and returns no node
blocks.

| Parameter | Adds | Cost |
| --- | --- | --- |
| (default, node scope) | `service`: request, search, ingest and background activity and latency; `memory`: indexing RAM budget, draining and pending-merge bytes, RAM storage, field metadata, jemalloc and its arenas, configured budgets. | Atomic loads and one allocator statistics read. |
| `collections=true` | `totals` and per-collection `collections`: documents, segments, committed bytes, commit time, generations, merges, inverters, snapshot pins and drops, query cache, availability. Implied on tenant and collection routes. | Briefly takes each writer's index lock and decodes each reader-only manifest. No file I/O. |
| `files=true` | `bytes` from an index directory listing; aux index and segment bytes. Implies `collections`. | Lists every index directory. |
| `segments=true` | Per-segment records. Implies `files`. | Iterates segments. |
| `system=true` | `system`: this process, the data filesystem and the process's cgroup v2. Node scope only. | About 15 procfs/cgroupfs reads and a scan of `/proc/self/fd`. |
| `host=true` | `host`: host-wide CPU, memory, network, TCP, disk and pressure statistics, which node_exporter also reports. Node scope only. | About 10 procfs reads. |
| `format=prometheus` | Text exposition instead of JSON (`text/plain; version=0.0.4`). | Rendering only. |

Flags accept exactly `true` or `false`; `format` accepts `json` or
`prometheus`. Any other value is a 400.

```bash
curl 'http://localhost:9400/_stats'                                  # cheap counters
curl 'http://localhost:9400/_stats?collections=true&system=true'     # everything without directory listings
curl 'http://localhost:9400/collections/docs/_stats?segments=true'   # one collection's layout
```

### JSON response

Counts follow the JSON dialect: a zero-valued field is omitted rather than
emitted, so a reader must treat an absent field as zero. Totals appear at the
node, collection, and index levels, and each level sets only the counts that
mean something there: `collections` only on the node total, `shards` only on
node and collection totals. A node-wide walk includes load-failure tombstones
with their `error` and excludes them from totals. A collection deleted during
the walk is reported the same way, as unavailable; one whose statistics cannot
be read for another reason carries that failure in its `error`, and the
failure is logged. Each collection index is sampled coherently, but a
node-wide response is not one transaction across collections.

Each serving collection also reports its `activity` since it was opened:
searches and index batches by outcome with their summed `seconds`, and the
documents and delete IDs its writer accepted or rejected. These count only
work that reached the collection. `service` has the node totals, which also
count requests that never resolved a collection and keep their counts after
a collection is deleted.

Two byte counts measure different things and will disagree:

- `committed_bytes` sums the file sizes recorded in the current published
  snapshot. It needs no filesystem access, so it is always reported. It
  excludes in-flight files and files kept only by snapshot pins.
- `bytes` (present only with `files=true`) is a directory listing of the
  whole index directory: manifests, in-flight flush and merge output, and
  pin-retained files. It is normally larger than `committed_bytes`, and can exceed the sum
  of segment bytes. With `segments=true` each segment reports its own `bytes`
  (data + deletes + overlays) and each aux entry reports the bytes of its
  listed files.

`service` counters are process-wide and reset on restart. Each activity
(`operations` per transport, `search`, `index_batches`, `flush`, `commit`,
`merge`, `vector_build`, `reader_refresh`) reports `in_flight`, completions by outcome
(`ok`, `error`, `cancelled`) and a `duration` histogram. Histogram `counts` are
cumulative, aligned with `service.request_duration_bounds` (requests, search
execution and index batches, from 10 microseconds) or
`service.background_duration_bounds` (the background stages, up to an hour),
in seconds, plus a final `+Inf` entry, and are omitted while `count` is zero.
A request or call fails when its response carries a whole-request error (a
failed search, an update that applied nothing), when a stream ends in
failure, or when the server fails it; a client that goes away first cancels
it. `errors` counts failed requests and calls by the kind of their first
error. An update that applies some of its documents succeeds; its rejected
documents are counted in `documents_rejected`, and those with an error of
their own in `document_errors` by kind. An NDJSON stream fails as a whole:
when it ends in failure, when its URL commit fails, or when it applied none of
its input. Messages on a gRPC `Search` or `UpdateStream` call count as
searches or index batches; the call fails only when it ends in failure.
`_stats`
requests themselves are counted under the `stats` operation; replication routes
are not counted.

In `memory`, `jemalloc` is present only when Luxir runs on jemalloc.
`jemalloc.allocated_bytes` counts live bytes allocated through jemalloc;
`resident_bytes` is jemalloc's upper bound including fragmentation and freed
pages awaiting decay; the process RssAnon also counts memory jemalloc does not
manage.

Ids and generations that appear in filenames use their filesystem spelling so
the response correlates directly with a directory listing: each segment's
`seg` is its data-file prefix (e.g. `s0a`), while `live_gen` and aux `gen`
are the sortable strings embedded in filenames (segment `s0a` with `live_gen`
`01` has its deletes in `s0a__L01`). These strings sort in generation order,
and an absent field means none. `schema_gen`, `index_gen`, `core_gen`, and
`update_version` are numeric. The schema and its history are embedded in each
`commit_<index_gen>` manifest.

### Prometheus scraping

Prometheus scrapes `/metrics` by default, so a job needs only its targets.
Parameters choose the cost: `collections=false` on nodes with very many
collections, and `host=true` only where node_exporter does not cover the host.

```yaml
scrape_configs:
  - job_name: luxir
    scrape_interval: 15s
    static_configs:
      - targets: ["localhost:9400"]
```

Prometheus supplies `up`, `job`, and `instance`. Aggregate service traffic over
instances; retain `instance` when diagnosing a specific node. Scrapes are
counted as `operation="stats"` traffic, so exclude that label when charting
client load.

### Service and engine metrics

| Metric or family | Meaning |
| --- | --- |
| `luxir_requests_total`, `luxir_requests_in_flight`, `luxir_request_duration_seconds` | HTTP requests or gRPC calls, by `transport`, bounded `operation`, and completion `outcome`. Duration includes body reception after HTTP headers, execution, waits, and response delivery. A gRPC stream is one call, regardless of its message count. |
| `luxir_request_errors_total` | Failed requests/calls, by transport and the fixed `kind` of their first error. An update that applies some documents is not a failed request. |
| `luxir_request_body_bytes_total`, `luxir_response_body_bytes_total` | Application payload throughput; excludes transport framing. Response bytes are counted when submitted for sending, even if delivery subsequently fails. |
| `luxir_searches_total`, `luxir_searches_in_flight`, `luxir_search_duration_seconds` | Searches the engine accepted, by `outcome`, from submission to the final response. A gRPC `Search` call can carry many searches, so this is its per-search view; `cancelled` means the search ended without a final response. |
| `luxir_search_execution_duration_seconds` | Search-body execution, excluding scheduler/visibility waits and later streaming drains. |
| `luxir_search_resource_exhausted_total`, `luxir_search_warnings_total` | Resource-limit failures (including the request memory breaker), and searches with declared degradation warnings. |
| `luxir_index_batches_total`, `luxir_index_batches_in_flight`, `luxir_index_batch_duration_seconds` | Index-batch processing inside the engine, excluding queueing and commit work. HTTP NDJSON can produce many batches per request. A batch fails only when none of it applied. |
| `luxir_ingest_documents_total`, `luxir_ingest_delete_ids_total` | Documents accepted into indexing or rejected/rolled back, and accepted delete IDs. Acceptance does not mean durability; commits can still fail. Deletes count submitted IDs, not matched documents. |
| `luxir_ingest_document_errors_total` | Documents rejected with an error of their own, by `kind`: `invalid_request` is bad input, `internal` an engine fault on that document. Documents rolled back with a failed batch have no error of their own. |
| `luxir_stream_pauses_total` | Response backpressure events by transport. |
| `luxir_http_connections`, `luxir_http_io_threads` | Live HTTP sessions and running connection-shard threads; the thread gauge excludes the accept thread. gRPC active calls are in `luxir_requests_in_flight`. |
| `luxir_background_total`, `luxir_background_in_flight`, `luxir_background_duration_seconds` | Flush, commit-finish, merge, vector-build, and reader-refresh stages by `kind`. These are stage attempts; a commit handing off to a forced merge can have multiple finish stages. |
| `luxir_node_memory_budget_bytes`, `luxir_search_request_memory_limit_bytes` | Configured managed-memory budget and tracked per-search ceiling. Neither is a process RSS limit. |
| `luxir_index_ram_*_bytes` | Indexing reservations, budget, draining reservations, and pending merge demand. The budget is soft; pending demand is capped at that budget when limited. |
| `luxir_storage_ram_bytes`, `luxir_storage_ram_limit_bytes` | RAM storage consumption and its cap, separate from filesystem usage. |
| `luxir_field_metadata_objects`, `luxir_field_metadata_bytes` | Cached field metadata by `part` (directory, page, entry, name). |
| `luxir_allocator_bytes`, `luxir_allocator_arena_*_bytes` | Jemalloc allocated, active, resident, mapped, retained, dirty, and metadata memory, plus indexing/big-buffer arenas. Resident is an allocator estimate, not process RSS. Absent without jemalloc; check `luxir_allocator_stats_available`. |
| `luxir_scrub_*_total`, `luxir_scrub_last_pass_end_timestamp_seconds` | Checksum scrub passes, files, read bytes, mismatches, and other check errors, and when the last pass ended. Present only while scrubbing is enabled; the timestamp is absent before the first pass. |
| `luxir_multiterm_expansion_refills_total` | Process-wide multi-term expansion refills. |
| `luxir_build_info`, `luxir_node_read_only` | Build version and CPU target, and read-only configuration. |
| `luxir_stats_duration_seconds` | Time gathering the response, excluding serialization. |

With `collections=true`:

| Metric or family | Meaning |
| --- | --- |
| `luxir_collections`, `luxir_collection_available` | Collection availability, including collections deleted or unreadable during the walk. An available HTTP endpoint does not imply every collection is usable. |
| `luxir_collection_storage_ram_bytes` | RAM storage owned by the collection. |
| `luxir_index_documents`, `luxir_index_segments`, `luxir_index_uncommitted_segments` | Flushed segment document/segment state; excludes documents still buffered in inverters. |
| `luxir_index_committed_bytes`, `luxir_index_last_commit_timestamp_seconds` | Current snapshot descriptor bytes and publication timestamp. An old timestamp alone does not indicate a problem on an idle collection. |
| `luxir_index_directory_bytes` | Directory-listing bytes; only with `files=true`. See the two byte counts above. |
| `luxir_index_commits_total` | Commits since the writer opened. |
| `luxir_collection_searches_total`, `luxir_collection_search_seconds_total` | Searches that reached the collection, by `outcome`, and their summed duration from submission to the final response. The mean is the ratio of the two rates. |
| `luxir_collection_index_batches_total`, `luxir_collection_index_batch_seconds_total` | The collection's index batches by `outcome`, and their summed processing time. |
| `luxir_collection_documents_total`, `luxir_collection_delete_ids_total` | Documents the collection accepted or rejected, and its accepted delete IDs. |
| `luxir_inverters`, `luxir_merge_tasks` | Busy, idle, and flushing inverters; outstanding merge work includes queued tasks. |
| `luxir_filter_cache_*` | Hit/miss counters, build attempts, builds, dead builds, thrash skips, admissions, rejections, evictions, purges, bypasses, reader-stable counters, payload/metadata memory, and payload budget. |
| `luxir_snapshot_pins`, `luxir_snapshot_retained_bytes`, `luxir_snapshot_pin_drops_total` | Current pins, retired files held only by pins, and pins dropped by `reason` (idle, budget). A stats request never expires pins or removes files. |

Counters reset when their owner restarts. Collection cache counters also reset
on recreation or explicit cache-control `reset_counters`; use `rate()` or
`increase()` rather than subtracting raw samples. Histograms use seconds and
cumulative buckets, including `+Inf`; compute fleet percentiles from aggregated
buckets, not averages of node percentiles.

Collection metrics use `tenant` and `collection` labels. There are no labels
for raw URLs, query text, field names, document IDs, commit IDs, or segment IDs.
Each collection adds about 50 series; on a node with very many collections,
scrape `/metrics?collections=false` and read per-collection detail from
`_stats` on demand.
Generations, aux indexes, and per-segment layout are JSON only.

### Process, system and host metrics

With `system=true` (Linux only), the response describes this process, its
data filesystem and its cgroup:

| Family | Coverage |
| --- | --- |
| `process_*` | CPU time, thread count, resident/virtual/anonymous/file/shared/swap/peak memory, open descriptors and soft descriptor limit, start time, voluntary/involuntary context switches, minor/major page faults, storage bytes, and I/O syscall bytes/counts. Context switches and CPU cover all process threads. |
| `luxir_data_filesystem_*` | The configured filesystem data directory's capacity, free/available bytes, total/free inodes, and read-only state. Only for filesystem storage. Arbitrary remote mounts are not probed. |
| `luxir_cgroup_*` | Current cgroup v2 memory usage/high/max, task count/limit, memory pressure/OOM events, CPU time/quota, and CPU throttling time/periods. Memory includes page cache and descendants. |

Cgroup statistics locate this process's membership using procfs and the
cgroup mount. They report **local** limits; ancestor cgroups can impose
stricter limits. A cgroup limit of `-1` means unlimited. Cgroup v1 is not
supported.

With `host=true` (Linux only), the response adds host-wide statistics, the
ones node_exporter reports. They are named `luxir_host_*` after their JSON
fields, so they never collide with node_exporter's series:

| Family | Coverage |
| --- | --- |
| `luxir_host_cpu_seconds_total`, `luxir_host_load*`, `luxir_host_procs_*`, `luxir_host_forks_total`, `luxir_host_context_switches_total`, `luxir_host_boot_time_seconds` | CPU modes per logical CPU, load averages, runnable/blocked tasks, task creation, context switches, and boot time. |
| `luxir_host_memory_*` | Total/available/free memory, page cache, swap, dirty/writeback pages, kernel/slab memory, faults, swap events, and OOM kills where supported. |
| `luxir_host_filefd_*` | Host-wide allocated file handles and their limit. |
| `luxir_host_network_*_total`, `luxir_host_tcp_*` | Per-interface bytes/packets/errors/drops and TCP established connections, retransmissions, resets, input errors, and failed attempts. These describe the network namespace, not just Luxir traffic. |
| `luxir_host_disk_*` | Every visible block device's read/write bytes, operations, merges, cumulative latency, I/O in progress, busy time, and weighted I/O time. Device and partition counters overlap; do not sum them indiscriminately. |
| `luxir_host_pressure_stall_seconds_total` | CPU, memory, and I/O pressure stall time, with `some`/`full` labels where supported. |

Inside a container, the host CPU, memory and disk figures describe the host,
not the container; network counters follow the container's network
namespace. The cgroup statistics under `system` are the container's own view.

Each section lists the collectors that read their sources in `available` and
the others in `unavailable` (JSON), reported as
`luxir_stats_collector_success{collector="..."}` 1 or 0. An unavailable
collector's measurements are omitted, not reported as zero. Restricted procfs,
disabled PSI, and absent cgroup controllers are common reasons.

Useful initial alerts are: scrape failures, unavailable collections, rising
internal/unavailable errors, p99 latency above the service target, sustained
indexing-memory pressure, filesystem byte/inode exhaustion, descriptor usage
near its limit, cgroup OOM events/throttling, and failed background stages.
For example, aggregate HTTP search p99 across nodes with:

```promql
histogram_quantile(0.99,
  sum by (le) (rate(luxir_request_duration_seconds_bucket{
    transport="http", operation="search"
  }[5m]))
)
```

There are no replication metrics in `_stats`; see
[replication status](replication.md#diagnose-status). Search admission queues and
deadline-driven partial results are not implemented on this revision, so no
statistics exist for those features.

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
assigned round-robin at accept and remain pinned to their shard.

Indexing, merges, and search work moved off a transport thread run on a shared
pool of worker threads. `--server.workers` sets its size. The default (`0`) is
one worker per available logical CPU less one, which leaves a CPU for the
transport threads; on machines with 4 or fewer logical CPUs it is one worker per
CPU, because holding one back would idle a large share of a small node (half of
a 2-vCPU one). Transport threads are not counted: they share the CPUs with the
workers.

```bash
./luxir --server.workers=2
```

Search requests default to serial execution directly on the transport thread
that received them (an HTTP io shard or gRPC completion-queue thread). This
avoids a scheduler handoff and idle-worker wakeups but occupies that
connection's thread for the query's duration. The request-level
`max_parallel` changes that: `1` moves the request onto the shared
work-stealing scheduler, still serial (use this for requests expected to be
expensive, so they do not delay other traffic on the same connection thread);
`-1` keeps the request on its transport thread but lets the scheduler's
workers join in with unlimited intra-request parallelism; values above `1` are
reserved for a bounded parallelism budget and not implemented yet. No setting
runs a request in parallel off its transport thread. Two paths leave the
transport thread whatever the setting: a request with `min_commit` continues on
a shared worker once its commit wait ends, and a gRPC search stream paused by
flow control resumes on one. The current gRPC unary update handler waits for
indexing on its completion-queue thread; streaming update and search use their
separate flow-control paths.

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

`--indexing.huge-pages` is enabled by default in jemalloc builds. It advises
transparent huge pages for whole-huge-page indexing allocations; disable it
with `--no-indexing.huge-pages` (or `--indexing.huge-pages=false`). This is a
best-effort hint: a host with THP set to `never` still works using ordinary pages.

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
  can submit concurrently. `0` (the default) uses the worker count
  (`--server.workers`) plus two; `1` makes batch submission serial. Reads pause when the limit is
  reached and resume as batches complete. A larger limit uses more staging
  memory.

gRPC has one request limit, on the size of each received message (a unary
request or one message of a stream):

```bash
./luxir --server.grpc.max-receive-message-size=64MB
```

It defaults to `--indexing.max-request-body`, so the buffered HTTP and gRPC
ceilings move together. A larger message fails with `RESOURCE_EXHAUSTED`
before it is decoded; see [gRPC API](grpc.md#message-size). Replies have no
size limit; search results are streamed in bounded batches.

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
  first. Cache counters appear in `_stats?collections=true`
  (and on the collection and tenant `_stats` routes). Date ranges with an unrounded
  `NOW` bound are never cached; see [Dates](dates.md#date-math).

## Time-zone data

IANA time-zone names require the system zoneinfo database and compatible C++
runtime data. Without it, date queries retain UTC and fixed-offset support but
named zones are unavailable. Keep the zoneinfo data and C++ runtime consistent
across machines if requests must produce identical civil-time boundaries.

## Shutdown and recovery

On `SIGTERM` or `SIGINT` the server stops accepting connections, waits for
in-flight gRPC and HTTP requests to finish, stops background work (scrubbing,
replication), releases the data directory and exits with status `0`. Updates
that were not yet committed are not committed by shutdown: only acknowledged
commits are durable, so before a planned stop quiesce producers and publish an
immediate commit. A second signal during the drain exits immediately with
status `128 + signal`. After an unplanned stop, the filesystem backend
reopens the last durable commit. If recovery selects an older valid root, a writer
publishes it under a new incarnation so followers cannot confuse it with an
older generation of their current source. File retirement does not fsync removals:
reappearing obsolete files are harmless and a later sweep reclaims them.

At startup, a collection whose top-level index metadata cannot be parsed is
kept as a tombstone rather than preventing healthy collections from loading.
Queries and updates to that collection return the recorded load error as an
`unavailable` error (HTTP `503`, code `collection_unavailable`) and the server
will not silently recreate storage over it. Other segment corruption is
detected when the affected reader is opened, or earlier with the
[file integrity checks](#file-integrity-checks) below.

## File integrity checks

Every committed manifest records each file it references with its size and
xxh3-64 digest, computed while the file is written. Checks read a file with
its own descriptor (never through the mapping queries use), so a bad sector
fails the check instead of the process, and a check never touches the request
threads' address space.

- **Transfers** always verify: a replicated file is hashed while it streams in
  and is installed only if it matches, and a follower hashes a leftover local
  candidate before reusing it. Reuse by hard link (a repointed follower,
  `luxir promote`, writer recovery from an older root) compares manifest
  descriptors only; whether the linked bytes still match is the at-rest
  question the checks below answer.
- **`--store.verify-on-open=full`** reads every file of every collection's
  committed snapshot at startup, one collection at a time on the node's
  indexing threads. Within a collection, files hash in parallel, largest first;
  checksum descriptors are held for at most one collection at a time. Checked
  files are verified before any reader maps or parses them. Readers map those
  same descriptors, so a file retired during hashing remains readable. The
  default, `none`, trusts the manifest footer and file sizes. A mismatching, missing or unreadable file fails only that
  collection: it loads as unavailable (HTTP `503`, code `collection_unavailable`)
  with an error naming the file (`checksum mismatch: <file>: expected size=..
  xxh3=.., actual ..`, `missing file: <file>` or `unreadable file: <file>: ..` for EIO),
  and its data is kept. Other verification errors also fail the load, with
  `could not verify file: <file>: ..`; verification never creates corruption
  markers. A follower then fetches the damaged files from its source. `luxir promote --store.verify-on-open=full` checks the files it links
  the same way. Startup reads the whole index, so expect it to take as long as
  reading the data directory once.
- **`--store.scrub-interval-s=N`** (default `0`, off) re-reads the files of
  every published snapshot every `N` seconds on one thread, with `O_DIRECT`
  where the filesystem allows so the pass reads the media and leaves the page
  cache to queries, paced to `--store.scrub-max-bytes-per-sec` (default
  `64MB`, `0` for unthrottled; the rate is the throttle, the thread's low CPU
  priority only yields the CPU). A size/digest mismatch, EIO, or a held file
  missing on a writer or follower is logged at error level and fails
  the collection: it is unavailable with the same error, a writer publishes
  nothing more over the damaged bytes (no further update, commit or merge), and
  the suspect file is recorded in a `corrupted_<id>` marker in the incarnation
  directory. `GET /_stats` reports node-wide `scrub` counters (`passes`,
  `files`, `bytes`, `mismatches`, `errors`, `last_pass_end` in Unix milliseconds)
  while scrubbing is enabled. `mismatches` includes EIO and unexpectedly missing
  files. Other errors (such as descriptor exhaustion, allocation failures or
  permission errors) are warnings counted in `errors`; they leave the collection
  available and the next pass retries. On a read-only node a live writer can
  retire files still served through mappings: the scrub skips missing files.

On a writable node a marker survives restarts: at startup the files it names
are re-verified with `O_DIRECT` where supported, whatever
`--store.verify-on-open` says. Under `full` other files use buffered reads.
The collection stays unavailable while a suspect file still fails; the marker
is removed once its files pass or are replaced. Read-only nodes honor existing
markers but cannot persist new ones or clear them, so their scrub failures do
not survive a restart unless another process recorded a marker.

Markers travel with `luxir promote` and writer recovery into a new incarnation,
restricted to files in the copied manifest. A promoted or recovered writer
therefore re-checks known suspects even under the default policy. A follower
repairs a scrub hit by itself: the next sync re-downloads every marker-named
file even if its buffered hash matches, checks other local candidates as usual,
and installs the copy again. Suspect files are never linked from the serving
copy, and a failed serving collection supplies no hard-link reuse. An advance
cannot replace a suspect serving file in place; repair first unloads the failed
collection and re-stages it. Repair a writer by restoring from a follower
(`luxir pull` and `luxir promote`) or by replacing the damaged file from a
backup and restarting.

## Current production boundary

Before treating Luxir as a production service, account explicitly for the
features it does not yet supply:

- no automatic failover, leader election, or distributed query execution;
- no built-in TLS/authentication/authorization;
- no scheduled backups or automatic restore (use `luxir pull` and `luxir promote`);
- pre-1.0 wire and schema interfaces that may change.

None of these can be enabled by configuration. The pieces those layers will
build on are already in place: immutable commits, isolated collections,
transport-independent request messages, and a node-wide scheduler and memory
model.
