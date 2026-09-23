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
