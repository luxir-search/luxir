# Storage tiers: lazy range reads

Luxir segments are immutable files read in place (see
[memory-mapped, zero-copy reads](architecture.md#memory-mapped-zero-copy-reads)).
A storage backend can also keep segments somewhere a read is a network request,
such as an object store (S3 and the like), and fetch them by byte range on first
touch, so that a node's local disk or RAM only holds what queries touched. The
core provides what such a backend needs from the read path; the backend itself
(the store client, staging, uploads, caching) is a plugin. This page describes
the core side and what it leaves to the backend.

Nothing here changes an existing file format. The mmap and RAM read paths are
unchanged in cost.

## The pieces

- **`InputFile::rangeStream` / `prefetchRanges` / `lazy`** (`store/OutputStream.h`):
  the reader contract. See [the open path](#the-open-path-contract).
- **`Directory::REMOTE`** (`store/Directory.h`): the capability of a directory
  whose bytes may be fetched over a network.
- **Hotcache** (`reader/Hotcache.h`): a per-segment file listing the ranges that
  opening the segment reads.
- **`Directory::knownFiles`** (`store/Directory.h`): the committed files of the
  manifest a directory was opened at, with their sizes.

## The open path contract

> Open-time reads go through `rangeStream`. Bulk reads materialize whole files on
> first touch.

`rangeStream(offset, length)` returns a stream over a byte range whose offsets
count from `offset`; its bytes stay valid as long as the `InputFile` lives, like
`getInputStream()`. `prefetchRanges` is advisory: the caller will read these
ranges soon. `lazy()` is true when bytes are fetched on first touch, so that
`read()`, `getInputStream()` and the first touch of a range may block on I/O and
throw.

`PostingsReader` opening a segment reads, per file, an 8 byte header; from file 0
the tail: the segment info, the field offsets and the field metadata (the
"catalog"); and the request versions, wherever the segment info points. These are
explicit ranges, not slices of an eagerly created whole-file stream. A segment's
catalog is kept as a stream of its own and `FieldReader` and `findOrdinal` read
field metadata from it, so looking up a field does not touch the rest of file 0.
The first lookup of a stored-fields or int-column field also reads its column
metadata, which fetches the file that holds it.

Everything else (postings, columns, stored fields, BKD points) is read through
`PostingsReader::getInputStream(fileNum)`. That stream is created on first use,
and on a lazy file creating it is what fetches the file. So a file no query
touches is never fetched. Files that are already resident (RAM, mmap) publish
their streams at open, and a use costs one acquire load.

`InputFile` defaults make every other implementation conform for free:
`rangeStream` slices `getInputStream()` and `prefetchRanges` calls `prefetch` (an
`madvise` on mmap files).

What this does not do is make a query read less than a whole file. A query that
needs any term of a field fetches every file holding that field's data in full.
Declaring the postings ranges a query will read (and fetching only those) is the
next step: the postings are block based and reached through the term dictionary,
so a reader could name the ranges it needs, but the readers do not declare any
yet. How well files separate fields depends on the writer: a segment's streams
are balanced by size, so one field's postings are spread over the segment's
files, while small fields and columns often stay together.

## The hotcache

Opening a segment costs a handful of dependent reads (the header of file 0, its
last four bytes, the segment info before them, the headers of the other files,
the versions header and then its blocks, the field count, the offsets table and
then the catalog). Against an object store each is a round trip. The hotcache records
them so a backend can fetch them together up front.

When a segment's files are finalized, at flush and at merge output
(`PostingsWriter::finish`), and the directory is `REMOTE`, the segment is opened
once through a `RecordingDirectory`: a `DelegatingDirectory` whose input files
record every range read through them. The recorded ranges are merged and rounded to
4 KiB and written as `s<seg>__hot`. Its format is the magic `LUXHOT01`, a format
version, a count, `(file number, offset, length)` triples as vints, and a trailing
xxh3 of everything before it. A failure to record is logged and leaves the
segment without a hotcache.

The name has two underscores so that every place that treats a segment's files
alike picks it up: `Manifest::indexFile`, the commit's file list, verify-on-open,
the scrub, replication (a follower receives it like any file) and the
`deletePrefix` that retires a dead segment. Local backends do not write one: their
open reads are local, so it would be dead weight.

Reading it is the backend's job: on `openFile` of a segment data file
(`Hotcache::parseDataFile`), a backend reads the segment's hotcache once
(`Hotcache::fileName`, `Hotcache::decode`) and calls `prefetchRanges` on the file
with `rangesOf(fileNum)`. A segment without a hotcache, or with one that does not
decode, opens with the same reads issued one after another; the hotcache is an
optimization and a damaged one should be ignored (verify-on-open and the scrub
still check the file itself). `HotcacheSegmentTest` pins that an open reads
nothing outside its hotcache.

## Known files

`Manifest::load` passes the committed files of the manifest it chose, with their
sizes, to `Directory::knownFiles` (the default does nothing, and every delegating
directory forwards it). It is a hint, separate from `expectSynced`, which asserts
the sync discipline `CheckedDirectory` enforces. A remote directory can use it in
place of asking its store: opening a committed file needs no size request, and a
segment listed without an `s<seg>__hot` has no hotcache, so its open need not
probe for one. Committed names are never rewritten with other bytes; a directory
that remembers them forgets a name it deletes, writes, publishes or links over.

## The request I/O context

A lazy read happens deep below the search that needs it, often on a worker
thread, so the storage layer cannot be handed the request through its
interfaces. Instead the search installs an `IoContext`
(`src/luxir/util/IoContext.h`) in a thread-local slot, and storage code reads
it with `IoContext::current()`. It carries the request's query id and
`request_id`, its deadline (the gRPC client deadline; none over HTTP), a fill
hint (`fillCache`: false for reads that should not displace cached data, such
as a scan) and the request's `IoCounters`.

The slot is set by an `IoScope`, a guard that restores the previous value when
it ends, also on unwind:

- `SearchEngine::submitBody` sets it for the request body, on whichever thread
  runs it (an HTTP io shard, an arena worker, a gRPC worker).
- `task_group_fanout` and `TaskGroupRunner::run` wrap each task they publish
  with `bindIoContext`: the task copies the spawner's context and installs it
  on the thread that runs it, then restores that thread's own. A worker that
  steals a task while waiting nests correctly, and a context never outlives
  its task.
- A streaming emitter resumed after backpressure installs the request's
  context again on the thread that resumes it.
- A `SharedLazyBuild` runs under `IoContext::shared()`: the reads count for the
  request that builds, but its deadline and fill hint do not decide a value
  other requests take.

A context is a small value; only its counters point at request-owned state,
which outlives every task and stream of the request. Reads with no context
(merges, startup, verification, replication) see null and behave as before;
the cost on the read path is one thread-local load.

Core carries the context but reads nothing over a network, so the counting is
the backend's: its lazy files count each store request and the bytes it
returned (`countFetch`), the bytes readers resolve (`countTouch`) and the
reads that need a whole file (`countWholeFileRead`), and its own reads for a
request (the hotcache, checksum streaming) count as requests too. The totals
appear in the request's profile and in the `search_io_*` service counters.
Core never hedges, gives up early or skips a cache on the deadline or the fill
hint either; acting on them is for a storage layer. Nothing in core sets
`fillCache` to false yet.

## What a remote backend provides

A backend plugin that sets `REMOTE` is expected to:

- return from `openFile` an `InputFile` that overrides `rangeStream`,
  `prefetchRanges` and `lazy()`, keeps every pointer it hands out valid for the
  file's lifetime (one stable mapping, with physical memory committed only for
  what was fetched, keeps the readers' plain-pointer access). `openFile`
  returns null for an absent name, as on every directory; a lazy fetch that
  later finds the object gone throws `std::filesystem::filesystem_error`;
- use the hotcache and `knownFiles` as above;
- count the store requests, fetched and touched bytes and whole-file reads it
  does for a request into `IoContext::current()`, as above;
- keep an object readable while a reader holds it: on a filesystem a merged-away
  segment stays readable for the queries that still hold it, because the unlink
  leaves the inode, so a delete of an object a reader holds should hide the name
  and remove the object when the last reader is gone.

Eviction, parallel fetches, multipart uploads and caching layers are the
backend's concern; the layer registry composes them (a `DelegatingDirFactory`
over the backend). See [plugins](../dev/plugins.md#remote-backends).
