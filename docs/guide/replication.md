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

A follower follows the whole source namespace and serves ordinary local searches.
Start it with an empty directory or an existing follower directory. Repointing
`--replicate-from` to another source reuses files whose name, size and xxh3 digest
match, including across incarnations. An existing data directory must be a
follower directory; ordinary writer data directories are refused. Updates, schema
changes and collection admin are rejected, except deleting an orphan (below).

New collections appear even when empty. A same-boot recreate replaces the old
incarnation, including with an empty snapshot. Across a source restart, a new
empty incarnation keeps a populated local copy serving until the source commits
data. A source collection that becomes unavailable never means deletion.

## Write, then read your write

```sh
curl 'http://writer:9400/collections/main/_update?commit=true&wait_for_replicas=all&replication_timeout_ms=30000' \
  -H 'Content-Type: application/x-ndjson' -d '{"id":"example"}'
# Use the returned commit token:
curl 'http://reader:9400/collections/main/_search?min_commit=INCARNATION:GEN&min_commit_timeout_ms=30000' \
  -H 'Content-Type: application/json' -d '{"query":"id:example"}'
```

A commit token is `INCARNATION:GEN`. `wait_for_replicas=N|all` commits immediately
and returns `commit` plus `replicas: {wanted, serving, timed_out}`. It also works
in JSON/NDJSON commit objects and gRPC. `all` captures live followers already
serving some commit of that collection's incarnation when the commit completes.
Watch-only and still-syncing followers are excluded; later arrivals do not join.
Captured followers that expire stop being required. A dead follower remains live
for `replication.follower-timeout-ms`, so `all` can time out before it expires.
A numeric N can exceed the live count and waits until timeout.

`min_commit` waits for the requested generation or a descendant of the same
incarnation, on either writers or followers. At timeout, an insufficient
snapshot returns 503 `stale_replica`; a different incarnation returns 409
`commit_incarnation_mismatch`. Writers/read-only nodes reject an incarnation
mismatch immediately; followers allow time for an incarnation switch.
Both timeout defaults are 30000 ms. Waits do not block later commits. A timeout
never rolls back a committed write. An abandoned request waits until its deadline.

An NDJSON URL EOF commit covers only collections touched by that stream. For
multiple collections its result is `commits: {name: {commit, replicas?}}`;
`replicas` appears only when requested. Single-collection results keep the shape
above. A floor also protects reads sent to followers excluded from captured `all`.

## Copy, restore or promote

```sh
luxir pull http://writer:9400 /data/seed
# Seed a follower:
luxir --replicate-from http://writer:9400 --store.backend=fs --store.data-dir=/data/seed
# Restore as an independent writer:
luxir --promote --store.backend=fs --store.data-dir=/data/seed
```

Pull starts no server. It prints each collection's commit, transferred/reused
bytes and a summary; errors give a nonzero exit. Re-running is incremental and
may use a different source URL. Membership is captured once, with each snapshot
fetched when reached; this is not an atomic namespace-wide backup. Absent local
collections are retained. Pull does not acknowledge serving traffic.

For manual failover, stop the old writer and the chosen FS follower, then start
that follower's directory with `--promote` and without `--replicate-from`.
Promotion gives every collection a new incarnation, reusing immutable files.
Restart other followers with `--replicate-from http://new-writer:9400` and their
existing directories. A failed promotion leaves that collection unavailable;
restart with `--promote` to finish. Completed promotions are not repeated.
Promotion requires FS and cannot be combined with following.
Use **pull plus promote** to restore, rather than copying a writer directory
back over a live identity. Writer recovery from an older valid root also mints
a new incarnation. Read-only nodes can open a follower directory directly.

## Diagnose status

`GET /_replication/status` is the replication status surface. On followers it
shows source, connection/last-contact information and per-collection source and
serving commits, byte progress, `last_error` and `next_retry` (Unix milliseconds).
It also lists live downstream followers, their serving commits and generation
lag. Follower state lives here; `_stats` retains writer-side reservation counters
and node/per-collection RAM storage usage.

| State | Meaning and action |
|---|---|
| `syncing` | Download/install in progress; the previous snapshot keeps serving. |
| `serving` | The advertised snapshot is installed. |
| `waiting` | A new source boot has an empty replacement for populated local data. Commit source data to replace it. |
| `stale` | Source unreachable or collection unavailable. Repair the source/network; local searches continue. |
| `orphan` | Name absent after a source boot change. Keep it, or delete it through the normal collection-delete API while connected. |
| `error` | Inspect `last_error`; fix storage/network/corruption, then let `next_retry` run (backoff 1-60 s). A backwards generation requires restoring/promoting the source under a new incarnation. |

Absence in the same source boot deletes the local collection. Absence after a
boot change leaves an orphan: a restart can hide a deletion. Failed installs
leave the old reader serving. Broken local replicas are discarded and fetched
again. Fully verified candidate files survive restart and are checked for reuse;
partial-file Range resume lasts only within a process.

Use `--store.backend=ram` for a RAM writer or follower. RAM data and generated
follower ids disappear on restart. A RAM writer starts new incarnations;
followers keep old populated data until the first data commit. RAM followers
refetch on restart. `--store.ram-limit-mb` (default 0, unlimited) bounds storage
buffers independently of indexing memory. Pressure first drops the oldest
reservations retaining retired files, excluding current snapshots and open file
transfers, then reports `storage_memory_limit` if space is still insufficient. Allow
room for current data plus changed files; old readers can delay reclamation.
Node/per-collection storage usage remains in `_stats`.

## Reference

All replication settings use `--replication.` (configuration keys use underscores).

| Setting | Default | Purpose |
|---|---:|---|
| `source` | empty | Source HTTP URL; `--replicate-from` is shorthand |
| `follower-id` | generated | Persisted FS follower identity |
| `downloads` | 2 | Concurrent collection transfers |
| `follower-timeout-ms` | 90000 | Live-follower window; watches are clamped to one third |
| `pin-idle-timeout-ms` | 60000 | Reservation expires without byte progress |
| `pin-retained-bytes` | 1 GiB | Per-collection retired bytes retained only by reservations |

Reservations are shared by commit and survive individual requests. The retained-byte
budget counts unique retired files held only by reservations; current/merge-owned
files do not count. Oldest reservations are revoked first when that budget is
exceeded. Snapshot acquisition and byte progress renew them; idle expiry or
revocation aborts their transfers. A server timer checks expiry; internal users
also expire reservations on activity. `_stats` exposes `snapshot_pins`,
`pin_retained_bytes`, `pin_idle_drops`, and `pin_budget_drops`. Reservations and
acks are process-local. Every HTTP socket write has a fixed 60 s idle deadline,
so one stalled client cannot hold a transfer indefinitely while another client
renews their shared reservation. Followers back off after a lost reservation.

Storage layout is `c/name/incarnation/`, with `CURRENT` containing
only the selected incarnation. `replication.json` stores follower identity,
last source URL and discovery/recovery state; it does not bind the source URL.

| Endpoint | Contract |
|---|---|
| `GET /_replication/watch?since=CURSOR&timeout_ms=30000&follower=ID` | Full `{boot, cursor, collections}` catalog; echo the opaque cursor. Unknown cursors return immediately. Requested timeout is clamped to one third of the source follower timeout; followers request one third of their own. |
| `GET /_replication/COLLECTION/snapshot` | Reserves current snapshot; binary manifest and `X-Luxir-Commit`. Add `?format=json` to inspect files/sizes/digests. HEAD has no side effects. |
| `GET /_replication/COLLECTION/file/NAME?commit=TOKEN` | File from a reservation; optional single byte Range (206/416), HEAD supported. Malformed/multiple ranges return full 200. Missing membership: 404 `file_not_in_snapshot`; expired reservation: 410 `snapshot_expired`. |
| `POST /_replication/installed` | JSON `{follower, collection, commit}` acknowledges a serving snapshot. |
| `GET /_replication/status` | Status described above. |

Snapshot/file GETs accept `follower=ID`; these, watches and installed acks renew
liveness. On 410, back off, fetch a new snapshot and reuse verified files. Transfers send
from the reserved mmap with advisory readahead and socket backpressure.
The catalog includes unavailable collections with `state: "unavailable"` and
last commit when known. Acknowledgments never move backwards within an incarnation.
