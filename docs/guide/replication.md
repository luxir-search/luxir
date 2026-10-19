# Replication, copies and failover

A follower keeps a verified, continuously updated copy of a writer's
collections and serves searches from it. Writes can wait until followers serve
them, and reads can wait for a given commit, so a client can read its own
writes from any node.

## Start a writer and a follower

```sh
# W1
luxir --store.backend=fs --store.data-dir=/data/writer
# R1, on another host
luxir --replicate-from http://writer:9400 --store.backend=fs --store.data-dir=/data/reader
curl http://reader:9400/_replication/status
```

A follower follows every tenant by default. Use
`--replication.tenants alpha,beta` to follow only those tenants.

Start a follower with an empty directory or an existing follower directory.
Repointing `--replicate-from` to another
source reuses files that already match. Followers reject updates, schema changes
and collection admin, except deleting an [orphan](#diagnose-status).

`--replication.peers http://reader2:9400,http://reader3:9400` lets a follower
fetch files from other nodes before the source, offloading the source's
bandwidth. Every file is verified against the manifest the source announced,
and only the source decides which commit is current.

New, recreated and deleted collections on the source are followed
automatically. A source that restarts without its data, such as a RAM writer,
does not empty its followers: a follower keeps serving its copy until the source
commits data to that collection.

If a local collection cannot be opened at startup, the follower keeps its files
and reports it unavailable. A later verified install can replace it. Until then,
the unreadable copy is treated as populated for the empty-replacement rule.
With `--store.verify-on-open=full` a local file whose bytes no longer match its
manifest digest also makes the copy unreadable, and a background scrub that
finds one does the same while the follower runs; the next install keeps the
files that still verify and re-downloads every file named by a corruption
marker, even when its buffered hash matches. Suspect files are excluded from
hard-link reuse from the serving copy; a failed serving collection supplies no
such reuse. A repair unloads the failed collection before replacing files in
its incarnation. See
[file integrity checks](operations.md#file-integrity-checks).

## Try it on one machine

Start a writer on port 9400 and a follower on port 9500, each in its own
terminal:

```sh
luxir -p 9400 --store.backend=fs --store.data-dir=/tmp/luxir/writer
luxir -p 9500 --replicate-from http://localhost:9400 --store.backend=fs --store.data-dir=/tmp/luxir/reader
```

The follower picks up the writer's default `main` collection:

```http
GET /_replication/status
Host: localhost:9500
```

```json
{
  "source": "http://localhost:9400",
  "follower": "myhost:9500",
  "connected": true,
  "last_contact": 1791237685529,
  "collections": [
    {
      "name": "main",
      "tenant": "t0",
      "source_commit": "1048a9465ba18ea6:1",
      "serving_commit": "1048a9465ba18ea6:1",
      "state": "serving"
    }
  ]
}
```

`follower` is the name the writer knows this follower by, its host and HTTP
port. Set `--replication.follower-id` when hostnames are not unique.

The examples below use these two nodes.

## Write, then read your write

Add a document on the writer and schedule a commit within one second:

```sh
curl 'http://localhost:9400/collections/main/_update' \
  -H 'Content-Type: application/json' \
  -d '{"docs": [{"id": "example"}], "commit": {"commit_within_ms": 1000}}'
```

The update returns without waiting for the commit. Wait about a second for
the commit and for the follower to pull the changes, then search the follower:

```sh
sleep 1
curl 'http://localhost:9500/collections/main/_search?query=id:example'
```

```json
{"commit":"1048a9465ba18ea6:2","docs":[{"id":"example"}]}
```

## Guaranteed visibility

Replication is asynchronous, so a follower can lag behind the writer. A commit
token, `INCARNATION:GEN`, identifies a published snapshot: commit,
create-collection and schema-update responses return the token they published,
and search responses return the token they served. There are three ways to
guarantee that a write is visible.

### 1. Immediate commit

An immediate commit on a writer guarantees that later searches on that writer
see it. The default `freshness_ms=0` selects the latest local commit; a
positive value allows an older view. A follower is not covered.

```sh
curl 'http://localhost:9400/collections/main/_update?commit=true' \
  -H 'Content-Type: application/json' -d '{"docs": [{"id": "example"}]}'
curl 'http://localhost:9400/collections/main/_search?query=id:example'
```

The update response carries the commit token, which the next two ways use:

```json
{"update_version": 2, "status": "ok", "commit": "1048a9465ba18ea6:3"}
```

### 2. Commit and wait for followers

Adding `wait_for_replicas` to an immediate commit returns only once followers
serve it:

```http
POST /collections/main/_update?commit=true&wait_for_replicas=all
Host: localhost:9400

{"docs": [{"id": "example2"}]}
```

```json
{
  "update_version": 3,
  "status": "ok",
  "commit": "1048a9465ba18ea6:4",
  "replicas": {"wanted": 1, "serving": 1, "outcome": "satisfied"}
}
```

`satisfied` means every counted follower already serves that commit or a newer
one, so a search on one of them sees the write. `all` counts the followers
serving that collection when the commit completes: one that is still syncing
or joins later is not covered, and reads sent to it should use `min_commit`
below. A dead follower counts until `replication.follower-timeout-ms` expires,
so `all` can time out.

The outcome is `satisfied`, `timed_out` or `cancelled`, and
`wait_for_replicas_timeout_ms` defaults to 30000. The commit is durable
whatever the outcome. To retry a wait that timed out, commit again with
`wait_for_replicas` and no documents: the new commit includes the earlier one.

Creating a collection or updating a schema takes the same parameters. For a
new collection, which no follower serves yet, `all` counts the live followers
of its tenant that already serve another collection:

```http
POST /collections/_create?wait_for_replicas=all
Host: localhost:9400

{"name": "products"}
```

```json
{
  "commit": "860d5c67e1670c94:1",
  "name": "products",
  "replicas": {"wanted": 1, "serving": 1, "outcome": "satisfied"}
}
```

In gRPC, `CreateCollectionRequest` and `SchemaRequest` carry the same fields.

### 3. Wait for a commit when reading

A read can do the waiting instead: pass a commit token as `min_commit` and the
search waits until the node serves that commit or a newer one.

```sh
curl 'http://localhost:9500/collections/main/_search?query=id:example&min_commit=1048a9465ba18ea6:4'
```

`min_commit` works on either a writer or a follower, and requires that
generation or a later one of the same incarnation, even if `freshness_ms`
allows an older view. `min_commit_timeout_ms` defaults to 30000; if the node is
still behind at timeout, the search returns 503 `stale_replica` instead of
stale results. A writer rejects a different incarnation immediately with 409
`commit_incarnation_mismatch`; a follower waits for the incarnation to change
and returns that error if it still differs at timeout.

Passing a response's token as the next request's `min_commit` also keeps reads
monotonic.

## Copy, restore or promote

`luxir pull` copies every collection from a running node into a directory,
without starting a server:

```sh
luxir pull http://localhost:9400 /tmp/luxir/seed
```

```text
main 1048a9465ba18ea6:4 transferred=263 reused=0
Pull: 1 installed, 0 failed, transferred=263 reused=0
```

Re-running it is incremental, and may use a different source. Each collection
is a consistent snapshot, but the collections are not captured at one instant.

The copy is a follower directory. Start it as a follower to catch up with the
source, or promote it to restore an independent writer:

```sh
# Seed a follower:
luxir -p 9600 --replicate-from http://localhost:9400 --store.backend=fs --store.data-dir=/tmp/luxir/seed
# Or, with no server running on it, restore it as a writer:
luxir promote /tmp/luxir/seed
luxir -p 9600 --store.backend=fs --store.data-dir=/tmp/luxir/seed
```

Promotion gives every collection a new incarnation while reusing its files.
Promotion and writer recovery preserve corruption markers for files in the
copied manifest. The new writer re-checks those suspects at startup even with
`--store.verify-on-open=none`; promotion does not erase a pending scrub hit.

The promoted copy starts an independent commit history: it may be older than
the previous writer, and future writes can diverge. A new incarnation keeps
its commit tokens distinct, so an old `min_commit` token cannot incorrectly
confirm a write that is missing from the promoted copy.

Followers repointed to the promoted writer can switch to this history even
if their local snapshot is newer. Generation numbers are compared only
within an incarnation. The protection against replacing populated data with
an empty source still applies.

The command prints each collection's new incarnation:

```text
main 46cc218822c58397
Promote: 1 promoted, 0 failed; promoted from follower of http://localhost:9400
```

If promotion fails partway, re-run it; the directory will not start as a writer
until every collection is promoted. To restore a backup, use pull plus promote
rather than copying a writer directory back over a live one. Read-only nodes can
open a follower directory directly.

For manual failover, stop the writer and the follower, promote the follower's
directory, and start it as a writer:

```sh
luxir promote /tmp/luxir/reader
luxir -p 9500 --store.backend=fs --store.data-dir=/tmp/luxir/reader
```

Restart other followers with `--replicate-from http://localhost:9500` and their
existing directories.

## Diagnose status

`GET /_replication/status` shows the follower's source and last contact, each
collection's source and serving commits, byte progress, `last_error` and
`next_retry` (Unix milliseconds), and on a source, its live followers and their
lag.

| State | Meaning and action |
|---|---|
| `syncing` | Download/install in progress; the previous snapshot keeps serving. |
| `serving` | The advertised snapshot is installed. |
| `waiting` | The source restarted without data for this collection; the follower keeps serving its copy. Commit data on the source to resume syncing. |
| `stale` | Source unreachable or collection unavailable. Repair the source or network; the follower keeps serving searches from its copy. |
| `orphan` | Local copy the follower will not sync or delete on its own. See below. |
| `error` | Inspect `last_error`, fix storage/network/corruption, and let `next_retry` run (backoff from `retry-ms`, doubling to 60 s). A generation that moved backwards requires restoring or promoting the source under a new incarnation. |

A follower deletes a local collection only when the source it has followed
since that source's last restart no longer lists it. A collection missing after
a source restart, or outside a narrowed `--replication.tenants`, becomes an
orphan instead. Delete orphans with `/tenants/TENANT/collections/_delete`;
excluded tenants can be deleted while disconnected, other orphans need the
source connected.

Use `--store.backend=ram` for a RAM writer or follower. RAM data is lost on
restart, and RAM followers refetch. `--store.ram-limit-mb`
(default 0, unlimited) bounds storage buffers. Under pressure, pins on retired
files are dropped first; if space is still short, writes fail with
`storage_memory_limit`. Allow room for current data plus changed files.
Per-collection storage usage is in `_stats?collections=true`; node storage usage
and its limit are in `_stats` under `memory.storage_ram`.

## Reference

All replication settings use `--replication.` (configuration keys use underscores).

| Setting | Default | Purpose |
|---|---:|---|
| `source` | empty | Source HTTP URL; `--replicate-from` is shorthand |
| `follower-id` | host:port | Name the source knows this follower by |
| `tenants` | empty (all) | Comma-separated tenant subscription |
| `peers` | empty | Comma-separated nodes serving file copies, tried before the source |
| `max-acknowledgments` | 262144 | Node-wide follower/collection acknowledgment budget |
| `downloads` | 2 | Concurrent collection transfers |
| `follower-timeout-ms` | 90000 | How long a silent follower still counts as live |
| `retry-ms` | 1000 | Follower delay before reconnecting to a lost source, and before the first retry of a failed sync |
| `pin-idle-timeout-ms` | 60000 | Pin expires without byte progress |
| `pin-retained-bytes` | 1 GiB | Per-collection retired bytes retained only by pins |

Copying a snapshot pins it so its files stay readable. A pin expires after
`pin-idle-timeout-ms` without progress, and the oldest pins are revoked when
retired files held only by pins exceed `pin-retained-bytes`. The follower then
fetches a new snapshot and reuses the files it already verified.
`_stats?collections=true` reports `snapshot_pins`, `pin_retained_bytes`, `pin_idle_drops` and
`pin_budget_drops`.

A source tracks up to 4096 live followers and `max-acknowledgments`
(follower, tenant, collection) rows. Beyond either, it returns 429
`too_many_followers` or `too_many_acknowledgments`; existing rows still advance.

| Endpoint | Contract |
|---|---|
| `GET /_replication/watch?since=CURSOR&timeout_ms=30000&follower=ID` | Long-polls the catalog `{boot, cursor, collections}`, one `{tenant, collection, commit, manifest_xxh3, available}` entry per collection. Repeat `tenant=` to restrict it; pass the returned cursor back. |
| `GET /tenants/TENANT/collections/COLLECTION/_snapshot` | Pins the current snapshot; binary manifest and `X-Luxir-Commit`. Add `?format=json` to inspect files, sizes and digests. |
| `GET /tenants/TENANT/collections/COLLECTION/_snapshot/files/NAME?commit=TOKEN` | A file from a pinned snapshot; single byte Range supported. 410 `snapshot_expired` when the pin is gone. |
| `GET /tenants/TENANT/collections/COLLECTION/_snapshot/files/NAME?size=BYTES&xxh3=HEX16` | The file with that identity from the current or a pinned snapshot, without pinning; 404 `file_not_held` otherwise. Peers serve followers this way. |
| `POST /_replication/installed` | `{follower, tenant, collection, commit}` acknowledges a serving snapshot. |
| `GET /_replication/status` | Status described above. |

Snapshot and file requests accept `follower=ID`, which keeps the follower live.
The message shapes are
[`ReplicationCatalog`](../reference/protobuf.md#message-luxir.replicationcatalog),
[`ReplicationInstalled`](../reference/protobuf.md#message-luxir.replicationinstalled)
and
[`ReplicationCollectionStatus`](../reference/protobuf.md#message-luxir.replicationcollectionstatus).

## Limits

- Luxir has no authentication, authorization or TLS. Restrict both ordinary
  APIs and replication endpoints to trusted networks.
- Failover is manual (`luxir promote`).
- The tenant subscription is fixed for the life of the process.
- `luxir pull` does not capture all collections at one instant.
- Pins and acknowledgments are process-local, and a partially downloaded file
  resumes only within the same follower process.
