# Replication, copies and failover

Luxir has no authentication or authorization. Restrict both ordinary APIs and
replication endpoints to trusted networks. Replication uses HTTP, without TLS.

## Start a writer and a follower

```sh
# W1
luxir --store.backend=fs --store.data-dir=/data/writer
# R1, on another host
luxir --replicate-from http://writer:9400 --store.backend=fs --store.data-dir=/data/reader
curl http://reader:9400/_replication/status
```

A follower serves ordinary local searches and follows every tenant by default.
Use `--replication.tenants alpha,beta` to follow only those tenants. The subscription
is fixed for the process; restart to change it. An empty setting follows all tenants.
Tenant names use the same lowercase naming rules as collections; empty elements
inside a nonempty list are rejected.
Start it with an empty directory or an existing follower directory. Repointing
`--replicate-from` to another source reuses files whose name, size and xxh3 digest
match, including across incarnations. An existing data directory must be a
follower directory; ordinary writer data directories are refused. Updates, schema
changes and collection admin are rejected, except deleting an orphan (below).

New collections appear even when empty. A same-boot recreate replaces the old
incarnation, including with an empty snapshot. Across a source restart, a new
empty incarnation keeps a populated local copy serving until the source commits
data. A source collection that becomes unavailable never means deletion.

If a local collection cannot be opened at startup, the follower keeps its files
and reports it unavailable. A later verified install can replace it. Until then,
the unreadable copy is treated as populated for the empty-replacement rule.

## Write, then read your write

```sh
curl 'http://writer:9400/collections/main/_update?commit=true&wait_for_replicas=all&wait_for_replicas_timeout_ms=30000' \
  -H 'Content-Type: application/x-ndjson' -d '{"id":"example"}'
# Use the returned commit token:
curl 'http://reader:9400/collections/main/_search?min_commit=INCARNATION:GEN&min_commit_timeout_ms=30000' \
  -H 'Content-Type: application/json' -d '{"query":"id:example"}'
```

A commit token is `INCARNATION:GEN`. The URL syntax
`wait_for_replicas=N|all` forces an immediate commit and returns `commit` plus
`replicas: {wanted, serving, outcome}`. JSON/NDJSON commit objects and gRPC use
one typed requirement: `"wait_for_replicas":{"count":2}` or
`"wait_for_replicas":{"all":{}}`. An explicit count of zero checks immediately.
`wait_for_replicas_timeout_ms` defaults to 30000; zero checks without parking.

The outcome is `satisfied`, `timed_out`, or `cancelled`. It is separate from
local commit success: timeout, client cancellation, server shutdown, and a
collection removed or recreated after the commit all preserve the durable token.
Cancellation never turns a successful local commit into an update error.
Completed update bodies are released before the visibility wait.

`all` captures live followers already serving some commit of that collection's
incarnation when the commit completes. Watch-only and still-syncing followers
are excluded; later arrivals do not join. Captured followers that expire stop
being required, as do followers whose next watch unsubscribes from the collection's
tenant. Subscription changes discard acknowledgments outside the new subscription;
re-inclusion requires a fresh acknowledgment and never rejoins an already captured
wait. A dead follower remains live for
`replication.follower-timeout-ms`, so `all` can time out before it expires.
A numeric N can exceed the live count and waits until timeout.

Retry visibility without committing again:

```sh
curl 'http://writer:9400/collections/main/_wait_for_replicas?wait_for_replicas=all&wait_for_replicas_timeout_ms=30000' \
  -H 'Content-Type: application/json' -d '{"commit":"INCARNATION:GEN"}'
```

The body may also supply `wait_for_replicas` and
`wait_for_replicas_timeout_ms`; URL values override them. gRPC exposes the same
operation as `Admin.WaitForReplicas`. The response is `{commit, replicas}`.
For standalone waits, `all` captures membership when the request starts.
Create-collection and schema-update responses return the snapshot they published.
Search response envelopes return the token of the reader actually used; pass it
as the next request's `min_commit` for monotonic reads.

`min_commit` waits for the requested generation or a descendant of the same
incarnation, on either writers or followers. At timeout, an insufficient
snapshot returns 503 `stale_replica`; a different incarnation returns 409
`commit_incarnation_mismatch`. Writers/read-only nodes reject an incarnation
mismatch immediately; followers allow time for an incarnation switch.
Both timeout defaults are 30000 ms. Waits do not block later commits. A timeout
never rolls back a committed write. Transport cancellation completes a parked
wait as cancelled. HTTP observes socket failures while parked without consuming
queued request bytes. A write-half-close is valid, so EOF alone is not proof of
cancellation. Ambiguous EOF, or a disconnect behind queued pipelined bytes, may
only be resolved by the next I/O operation or wait deadline.

An NDJSON URL EOF commit covers only collections touched by that stream and
always returns `commits: {name: {commit?, replicas?, error?}}`, even for one
collection. An empty stream commits the URL's collection. `replicas` appears
only when requested. A failed local commit sets that collection's `error`;
replica wait outcomes preserve the durable `commit` token and local status. Every
admitted outcome is returned. Any collection failure sets the overall
`status: "error"` and a top-level `error`, with HTTP 200, as for a unary commit
failure. Unary updates keep `commit`/`replicas`. A floor also protects reads
sent to followers excluded from captured `all`.

## Copy, restore or promote

```sh
luxir pull http://writer:9400 /data/seed
# Seed a follower:
luxir --replicate-from http://writer:9400 --store.backend=fs --store.data-dir=/data/seed
# Restore as an independent writer:
luxir promote /data/seed
luxir --store.backend=fs --store.data-dir=/data/seed
```

Pull starts no server. It prints each collection's commit, transferred/reused
bytes and a summary; errors give a nonzero exit. Re-running is incremental and
may use a different source URL. Membership is captured once, with each snapshot
fetched when reached; this is not an atomic namespace-wide backup. Absent local
collections are retained. Pull does not acknowledge serving traffic.

For manual failover, stop the old writer and the chosen FS follower, run
`luxir promote DIR` on that follower's directory, then start it as a writer
without `--replicate-from`. Promotion starts no server. It gives every
collection a new incarnation, reusing immutable files, and prints each
collection's new incarnation and a summary. Any failure gives a nonzero exit and
keeps the directory follower-bound: a writer refuses to start on it, so no
collection serves until all are promoted. Re-run `luxir promote` to finish;
completed promotions are not repeated. Restart other followers with
`--replicate-from http://new-writer:9400` and their existing directories.
Use **pull plus promote** to restore, rather than copying a writer directory
back over a live identity. Writer recovery from an older valid root also mints
a new incarnation. Read-only nodes can open a follower directory directly.

## Diagnose status

`GET /_replication/status` is the replication status surface. On followers it
shows source, connection/last-contact information and per-collection source and
serving commits, byte progress, `last_error` and `next_retry` (Unix milliseconds).
It also lists live downstream followers, their serving commits and generation
lag. Follower state lives here; `_stats` retains writer-side pin counters
and node/per-collection RAM storage usage.

| State | Meaning and action |
|---|---|
| `syncing` | Download/install in progress; the previous snapshot keeps serving. |
| `serving` | The advertised snapshot is installed. |
| `waiting` | A new source boot has an empty replacement for populated local data. Commit source data to replace it. |
| `stale` | Source unreachable or collection unavailable. Repair the source/network; local searches continue. |
| `orphan` | Local copy outside the tenant subscription, or name absent after a source boot change. Excluded copies can be deleted while disconnected; other orphans require a connected source. |
| `error` | Inspect `last_error`; fix storage/network/corruption, then let `next_retry` run (backoff 1-60 s). A backwards generation requires restoring/promoting the source under a new incarnation. |

Automatic deletion requires current subscription membership, absence from a complete
catalog for that tenant, and the same remembered source boot. Absence after a
boot change leaves an orphan: a restart can hide a deletion. Narrowing the filter
makes excluded local copies orphans immediately, even while disconnected. They
are never synced, acknowledged or automatically deleted. Their source-boot and
empty-replacement history is retained, so re-inclusion resumes normal recovery
rules. Use `/tenants/TENANT/collections/_delete` to delete an excluded copy.
Failed installs leave the old reader serving. Broken local replicas are discarded
and fetched again. Fully verified candidate files survive restart and are checked for reuse;
partial-file Range resume lasts only within a process.

Use `--store.backend=ram` for a RAM writer or follower. RAM data and generated
follower ids disappear on restart. A RAM writer starts new incarnations;
followers keep old populated data until the first data commit. RAM followers
refetch on restart. `--store.ram-limit-mb` (default 0, unlimited) bounds storage
buffers independently of indexing memory. Pressure first drops the oldest
pins retaining retired files, excluding current snapshots and open file
transfers, then reports `storage_memory_limit` if space is still insufficient. Allow
room for current data plus changed files; old readers can delay reclamation.
Node/per-collection storage usage remains in `_stats`.

## Reference

All replication settings use `--replication.` (configuration keys use underscores).

| Setting | Default | Purpose |
|---|---:|---|
| `source` | empty | Source HTTP URL; `--replicate-from` is shorthand |
| `follower-id` | generated | Persisted FS follower identity |
| `tenants` | empty (all) | Comma-separated tenant subscription; immutable until restart |
| `max-acknowledgments` | 262144 | Node-wide follower/collection acknowledgment row budget |
| `downloads` | 2 | Concurrent collection transfers |
| `follower-timeout-ms` | 90000 | Live-follower window; watches are clamped to one third |
| `pin-idle-timeout-ms` | 60000 | Pin expires without byte progress |
| `pin-retained-bytes` | 1 GiB | Per-collection retired bytes retained only by pins |

Pins are shared by commit and survive individual requests. The retained-byte
budget counts unique retired files held only by pins; current/merge-owned
files do not count. Oldest pins are revoked first when that budget is
exceeded. Snapshot acquisition and byte progress renew them; idle expiry or
revocation aborts their transfers. A server timer checks expiry; internal users
also expire pins on activity. `_stats` exposes `snapshot_pins`,
`pin_retained_bytes`, `pin_idle_drops`, and `pin_budget_drops`. Pins and
acks are process-local. Replication file transfers have a fixed 60 s idle deadline
per socket write, so one stalled client cannot hold a transfer indefinitely while
another client renews their shared pin. Followers back off after a lost
pin.

Storage layout is `c/tenant/name/incarnation/`, with `CURRENT` containing
only the selected incarnation. `replication.json` stores follower identity,
last source URL and discovery/recovery state; it does not bind the source URL.

| Endpoint | Contract |
|---|---|
| `GET /_replication/watch?since=CURSOR&timeout_ms=30000&follower=ID` | Full catalog for the subscription (`tenant=alpha&tenant=beta`, omit for all), one `{tenant, collection, commit, available}` entry per collection in `{boot, cursor, collections}`; echo the opaque node-wide cursor. Publications in other tenants neither wake the watch nor force an immediate response. Foreign-boot, malformed and future cursors return immediately. Empty/invalid tenant parameters are rejected. Requested timeout is clamped to one third of the source follower timeout; followers request one third of their own. |
| `GET /tenants/TENANT/collections/COLLECTION/_snapshot` | Pins the current snapshot; binary manifest and `X-Luxir-Commit`. Add `?format=json` to inspect files/sizes/digests. HEAD has no side effects. |
| `GET /tenants/TENANT/collections/COLLECTION/_snapshot/files/NAME?commit=TOKEN` | File from a pin; optional single byte Range (206/416), HEAD supported. Malformed/multiple ranges return full 200. Missing membership: 404 `file_not_in_snapshot`; expired pin: 410 `snapshot_expired`. |
| `POST /_replication/installed` | JSON `{follower, tenant, collection, commit}` acknowledges a serving snapshot. |
| `GET /_replication/status` | Status described above. |

Sources allow up to 4096 live follower identities, independently of the
acknowledgment budget. Each acknowledged `(follower, tenant, collection)` consumes
one row. Watch-only followers consume no acknowledgment rows. At the budget,
existing rows can still advance, while new rows return HTTP 429
`too_many_acknowledgments`. Follower capacity returns 429 `too_many_followers`.
Expiry, unsubscription, collection deletion and incarnation replacement free rows.
An acknowledgment outside the follower's latest watch subscription is rejected.

Snapshot/file GETs accept `follower=ID`; these, watches and installed acks renew
liveness. On 410, back off, fetch a new snapshot and reuse verified files. Transfers send
from the pinned mmap with advisory readahead and socket backpressure.
The catalog uses the `ReplicationCatalog` proto message. Available entries always
carry a commit; unavailable entries may omit it. Followers reject malformed entries
and duplicate collection identities before applying any catalog changes.
Installed acknowledgments use `ReplicationInstalled`; status states are the `ReplicationCollectionStatus.State`
enum, rendered as lowercase JSON names. Acknowledgments never move backwards within an incarnation.
