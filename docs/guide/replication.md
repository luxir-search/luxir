# Index replication

Every HTTP server exposes these endpoints with the same access controls as its
other APIs. All public commit identities use one string: `INCARNATION:GEN`.
Recreation changes the incarnation; GEN is the manifest's `index_gen`. Update
responses, stats, replication headers, URLs and acknowledgments use this token.

Discover snapshots and keep a follower live:

```sh
curl 'localhost:9400/_replication/watch?timeout_ms=0'
curl 'localhost:9400/_replication/watch?follower=reader-a&since=BOOT:REVISION&timeout_ms=30000'
```

The response is `{boot, revision, collections}`. `collections` maps names to
`{commit: "INCARNATION:GEN"}`. Build the next cursor as `boot:revision`. An absent,
foreign, old or unknown cursor returns the full current catalog immediately.
The current cursor waits for publish/create/delete or timeout. The server clamps
`timeout_ms` to its configured maximum. Followers send 30000 ms and allow ten
extra seconds for the HTTP response. `follower` is optional for inspection;
followers send their stable id on every watch.

A follower remembers the source boot in which it last saw each collection. If a
catalog from that same boot lacks the name, it deletes the local copy. An absent
name after a different boot becomes an orphan and keeps serving. A delete
followed by a source restart before discovery is therefore indistinguishable
from a RAM source that has not rebuilt yet: it leaves an orphan. There is no
tombstone history or deletion cursor to retain.

A follower permits one collection-admin operation: explicitly deleting a local
orphan whose name is absent from the source's current catalog. Other collection
admin and all indexing/schema changes remain disallowed on followers. Follower
installation and this admin restriction are implemented by the follower mode.

Reserve and inspect a snapshot:

```sh
curl -D snapshot.headers localhost:9400/_replication/main/snapshot -o manifest
curl 'localhost:9400/_replication/main/snapshot?format=json'
curl -I localhost:9400/_replication/main/snapshot
curl 'localhost:9400/_replication/main/file/s01_00?commit=INCARNATION:GEN' -o s01_00
curl -H 'Range: bytes=1048576-' \
  'localhost:9400/_replication/main/file/s01_00?commit=INCARNATION:GEN' -o remainder
```

Binary is the default: the IndexInfo manifest includes schema and file names,
sizes and xxh3-64 digests, without the local disk footer. `format=json` renders
that proto as JSON. `X-Luxir-Commit` identifies the reservation. Snapshot and file
endpoints support HEAD. HEAD snapshot has no reservation or liveness side effects. File GET supports one byte range, including open-ended
and suffix ranges: 206 with Content-Range, or 416 if no bytes are satisfiable.
Malformed, multi-range and non-bytes ranges are ignored and return the full file
with 200. Oversized last-byte positions are clamped to EOF. Content-Length is the
response length (the corresponding GET length for HEAD).

Reservations survive individual requests and are shared by all clients of a
commit. Fetching the snapshot renews its reservation; byte progress renews it at
most once per second. Expiry, budget eviction, release or collection closure
aborts open transfers. Writes also have an idle deadline, so a non-reading client
cannot retain mapped files forever. An aborted response may be truncated; retry
with Range only while the reservation remains valid. A later request gets 410
for an expired/dropped reservation. On 410, fetch the newest snapshot and reuse
verified files whose name, size and digest match. A file outside a live snapshot
gets 404 `file_not_in_snapshot`; an unknown collection gets 404.

Report the snapshot a follower serves:

```sh
curl -H 'Content-Type: application/json' localhost:9400/_replication/installed \
  -d '{"follower":"reader-a","collection":"main","commit":"INCARNATION:42"}'
curl localhost:9400/_stats
```

Watch, snapshot and file GET requests accept `follower=ID` and renew follower
liveness; installed requests renew it too. Stats include only live
followers as rows `{follower, collection, commit, last_seen, lag}`; a follower
that has only watched has no collection/commit yet. `last_seen` is Unix
milliseconds. `lag` is the current generation minus the installed generation,
not a document count or elapsed time. Deletion or incarnation change drops that
collection's acknowledgment rows, while the follower stays live until its
liveness window expires. Stale rows are evicted first; the 4096-row
table rejects new rows with 429 only when all retained rows are live. This live
set is the one used by `all`: watch-only followers count as live but are not
yet serving. Older installed acknowledgments cannot move a follower backward.

Settings (CLI prefix `--server.replication.`):

| Setting | Default | Meaning |
|---|---:|---|
| `pin-idle-timeout-ms` | 60000 | Reservation idle timeout; expiry timer runs every half timeout |
| `pin-retained-bytes` | 1 GiB | Per-collection budget for unique retired bytes retained only by reservations |
| `write-idle-timeout-ms` | 30000 | Maximum idle time for each socket write; must be less than pin idle timeout |
| `watch-timeout-ms` | 30000 | Default and maximum watch timeout |
| `follower-liveness-periods` | 3 | Liveness window in maximum watch periods (90 seconds by default, minimum 2 periods) |

The current snapshot and files still owned by indexing/merges do not count toward
the retained-byte budget. Over budget, oldest reservations are revoked first.
One server timer schedules expiry on the task arena, including on idle nodes.
Reservations, acknowledgments and discovery cursors are process-local.

## Running a follower

```sh
luxir --replicate-from http://writer:9400 --store.backend=fs --store.data-dir=reader-data
curl localhost:9400/_replication/status
curl localhost:9400/_stats
```

`replication.source` follows the source's entire namespace. The follower takes
its normal data-directory write lock and starts only with an empty directory or
one already bound to that URL. The binding and generated follower id persist on
FS; generated RAM follower ids are new each process. `--replication.follower-id` optionally
sets the initial id. `--replication.downloads` bounds concurrent collection
transfers per node (default 2). One independent long-poll keeps discovery and
liveness active during downloads. Source URLs use HTTP and may include a port;
paths and credentials in the URL are not supported.

Followers construct no index writers and do not auto-create `main`. Searches
use ordinary local readers. Updates, schema changes and collection creation are
rejected. The normal delete-collection API accepts an orphan only after a
successful, currently connected discovery reports its name absent. Same-boot
absence deletes automatically; different-boot absence leaves an orphan.

A never-populated source collection does not appear locally. Its first physical
publication makes it eligible; create, empty commit and schema-only publications
alone do not. A later empty snapshot of an index that has had physical data is
eligible. After a source incarnation changes, the follower keeps serving the old
copy until the new incarnation qualifies and is completely installed. Ineligible
collections report `waiting`, including never-populated collections.

Files are reused by name, size and xxh3 digest. Connection failures retry with
Range from the partial offset within the current process; a lost reservation
restarts from the latest snapshot and keeps fully verified files. Partial-file
resume across process restarts is not supported. Downloads and checksums run on
bounded dedicated workers, outside search/indexing workers. Each worker reuses a
connection and I/O context. Ready collections run longest-waiting first. Failures
back off per collection from one second to at most 60 seconds. A lost reservation
restarts immediately unless it repeats without verified-file progress; checksum
failures retain the other verified files. RAM uses the same
Directory operations; known-size allocation and storage memory limits are separate
work.

An installer syncs data and directory entries, opens the candidate reader, then
writes and syncs its local manifest before swapping readers and acknowledging.
Writers and followers share `c/<name>/<incarnation>/`. The collection's `CURRENT`
file selects its incarnation and records the last observed source boot. It is
written, fsynced, atomically renamed and directory-fsynced only after the new
root is durable. An interrupted new incarnation leaves the old selection intact.
A follower directory opens on a writer with `--promote`, or directly on a
`--read-only` node. The node-level `replication.json` holds only the source URL and follower id.

Obsolete files retire through the snapshot registry. Replacing an incarnation
cancels old reservations and removes the old directory; admitted searches keep
their old reader manager and mapped files. Startup discards broken local follower
collections and fetches them again. Same-incarnation snapshots whose generation
is not newer than the serving snapshot are refused with `source went backwards`.

Status reports the source URL, follower id, connection state and last contact
(Unix milliseconds). Each collection reports `source_commit`, `serving_commit`,
`state` (`syncing`, `serving`, `waiting`, `stale`, `orphan`, or `error`),
`bytes_downloaded`, `bytes_total`, `last_error`, and `next_retry` (Unix milliseconds,
zero when no retry is scheduled). `serving_commit` identifies the old copy still
serving during a download or while waiting for a new incarnation. Byte progress
counts network bytes for the current attempt; total includes reused files. A
failed transfer leaves the old reader serving. Inspect and explicitly delete any
unwanted orphans.

Manual failover: stop the old writer, stop a follower, and start a writer on that
follower's data directory with `--promote` (omit `--replicate-from`). A writer
refuses a follower-bound directory without this explicit flag. Promotion republishes every
collection still marked as a follower in `CURRENT` under a fresh incarnation,
including empty collections. A failed collection stays unavailable; restart with
`--promote` to retry it. Already promoted collections keep their identity. The
source binding is removed only after all collections succeed, and the node logs
`promoted from follower of <url>`. With no binding, `--promote` has nothing to do.
Point the other
followers at the new writer using fresh data directories; an existing follower
binding deliberately rejects a different URL. A stable source URL can instead
be repointed to the promoted writer. Followers reuse matching immutable files
across incarnations (hard links on FS, shared buffers on RAM), downloading only
files whose name, size or digest changed.
A read-only node opens the same directory without promotion.

Discovery includes unavailable source collections with `state: "unavailable"`
and their last commit token when known. Followers retain their local copy and
report `stale`; unavailability is never interpreted as deletion. Reused HTTP
connections retry once fresh on EOF, connection reset or broken pipe before any
response bytes arrive. Read timeouts count as failures.
Orphan deletion fails immediately while a transfer owns the collection.

For read-your-write, commit with a visibility barrier and carry its commit token
on searches. FS and RAM followers count equally. These waits are asynchronous;
a slow replica does not delay later commits. A descendant commit of the same
incarnation satisfies either wait.

```sh
curl -s 'http://writer:9400/collections/main/_update?commit=true&wait_for_replicas=all&replication_timeout_ms=30000' \
  -H 'Content-Type: application/x-ndjson' -d '{"id":"example"}'
# Copy the response's commit token into min_commit:
curl -s 'http://reader:9400/collections/main/_search?min_commit=INCARNATION:GEN&min_commit_timeout_ms=30000' \
  -H 'Content-Type: application/json' -d '{"query":"id:example"}'
```

The NDJSON `_end_` commit object, JSON update `commit` object and gRPC CommitParams
accept the same options. `wait_for_replicas` is a decimal count or `"all"` (JSON
also accepts an integer). It overrides `commit_within_ms`. The response includes
`commit` and `replicas: {wanted, serving, timed_out}`. `all` captures live follower
ids when the commit completes, including watch-only followers not yet serving;
a follower that becomes live later does not join that wait. Captured followers
that expire stop being required and are excluded from `wanted`. A numeric count
can exceed the live follower count and will then time out. Timeout reports partial
progress and never undoes the local commit. Shutdown or client cancellation also
leaves the commit successful, with `replicas.cancelled: true` if a response can
still be delivered. A dead follower remains live for the
liveness window, so `all` can time out until it expires; lower the configured
window if this matters for your latency requirements.

`min_commit` works on writers, read-only nodes and followers, in the search body,
URL parameters and gRPC. The requested freshness is used when the reader already
satisfies the floor; otherwise the reader is refreshed. `min_commit_timeout_ms`
controls the search wait; `replication_timeout_ms` controls commit barriers.
Both default to 30000 ms; zero checks immediately. Waits are event-driven.

An unavailable generation at the deadline returns 503 `stale_replica`. Writers
and read-only nodes immediately return 409 `commit_incarnation_mismatch` for a
different incarnation. Followers wait for an incarnation switch, returning 409
only if the incarnation still differs at the deadline. Cancellation ends a wait
promptly. Collection deletion ends writer/read-only floors and commit barriers;
a follower floor can wait through deletion and recreation.

A URL EOF commit covers only collections touched by that NDJSON request. An
empty commit request commits its URL collection. One collection retains the
usual `commit` and optional `replicas` response. With multiple collections,
`commits` has a result for each; `replicas` is absent unless a replica wait was
requested:

```json
{"commits":{"main":{"commit":"INC:GEN","replicas":{"wanted":2,"serving":2,"timed_out":false}},"other":{"commit":"INC:GEN","replicas":{"wanted":2,"serving":1,"timed_out":true}}}}
```
