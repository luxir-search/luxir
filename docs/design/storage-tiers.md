# Storage tiers: lazy reads over an object store

Luxir segments are immutable files read in place (see
[memory-mapped, zero-copy reads](architecture.md#memory-mapped-zero-copy-reads)).
A node can also keep its segments in an object store (S3 and the like) and read
them by byte range, so a collection costs object-store prices and a node's local
disk or RAM only holds what queries touched. This page describes how that works
and what it does not do yet.

Nothing here changes an existing file format. The mmap and RAM read paths are
unchanged in cost.

## The pieces

- **`ObjectStore`** (`store/ObjectStore.h`): the few operations an object store
  offers: `list(prefix)`, `size`, `readRange`, `put` (optionally conditional),
  `remove` and a server-side `copy`. `RamObjectStore` is the in-memory
  implementation, with request counters and a request log so tests can assert
  what a read cost.
- **`InputFile::rangeStream` / `prefetchRanges`** (`store/OutputStream.h`): the
  reader contract. See [the open path](#the-open-path-contract).
- **`LazyInputFile`** (`store/LazyInputFile.h`): an `InputFile` over one object
  that fetches ranges on first touch.
- **`ObjectStoreDirectory` / `ObjectStoreDirFactory`**
  (`store/ObjectStoreDirectory.h`): a `Directory` and a storage backend over an
  `ObjectStore`.
- **Hotcache** (`reader/Hotcache.h`): a per-segment file listing the ranges that
  opening the segment reads.

## The lazy file

A `LazyInputFile` maps the object's full size as anonymous, unreserved memory
(`MAP_NORESERVE`), so every pointer it hands out points into one stable mapping.
That keeps the central property of the read path: readers hold plain pointers and
never check for residency per access, and "pointers stay valid as long as the
file lives" holds as it does for mmap. Physical memory is only committed for
chunks that were fetched.

A bitmap records which fixed-size chunks are resident (`chunk_bytes`, default
1 MiB). `rangeStream(offset, length)` makes the covering chunks resident, with
one `readRange` per contiguous run of missing chunks, and returns a stream over
the mapping. Concurrent readers fetch a chunk once: a fetch holds the lock
stripes of the chunks it covers (taken in ascending order), then looks again
before fetching, so readers of unrelated ranges do not wait for each other.
`prefetchRanges` merges its ranges in chunk space, fetches the chunks between two
ranges when the gap is small (64 KiB: a few extra bytes cost less than another
request), and is advisory: a failure is logged and the read that needs the bytes
fetches again and reports. `read()` and `getInputStream()` make the whole file
resident. A fetch of an object that does not exist (`ObjectNotFoundError`)
throws `std::filesystem::filesystem_error`, as a missing file would. Other
failures pass through as the store reports them: a failed request (a timeout,
throttling, a server error) is an ApiError such as `FileIOException`, which a
later read retries, and an object that does not match the read (a short read)
is a plain `std::runtime_error`.

The mapping reserves address space for the whole file, not memory, so it needs
the kernel's default overcommit policy (a strict `vm.overcommit_memory=2` counts
the reservation).

A fetched chunk is never evicted: its memory is released when the file is
destroyed. A collection that is queried broadly ends up with its touched files
resident, like a page cache that never drops pages.

## The open path contract

> Open-time reads go through `rangeStream`. Bulk reads materialize whole files on
> first touch.

`PostingsReader` opening a segment reads, per file, an 8 byte header, and from
file 0 the tail: the segment info, the request versions, the field offsets and the
field metadata (the "catalog"). These are explicit ranges now, not slices of an
eagerly created whole-file stream. A segment's catalog is kept as a stream of its
own and `FieldReader` and `findOrdinal` read field metadata from it, so looking up
a field does not touch the rest of file 0.

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
yet. How well files separate fields depends on the writer: a
segment's streams are balanced by size, so one field's postings are spread over
the segment's files, while small fields and columns often stay together.

## The hotcache

Opening a segment costs a handful of dependent reads (the last four bytes of file
0, then the segment info before it, then the offsets table, then the catalog, then
the versions). Against an object store each is a round trip. The hotcache records
them so they are fetched together up front.

When a segment's files are finalized, at flush and at merge output
(`PostingsWriter::finish`), and the directory is `REMOTE`, the segment is opened
once through a `RecordingDirectory`: a `DelegatingDirectory` whose input files
record every range read through them. The recorded ranges are merged and rounded to
4 KiB and written as `s<seg>__hot`. Its format is the magic `LUXHOT01`, a format
version, a count, `(file number, offset, length)` triples as vints, and a trailing
xxh3 of everything before it.

The name has two underscores so that every place that treats a segment's files
alike picks it up: `Manifest::indexFile`, the commit's file list, verify-on-open,
the scrub, replication (a follower receives it like any file) and the
`deletePrefix` that retires a dead segment. Local backends do not write one: their
open reads are local, so it would be dead weight.

`ObjectStoreDirectory::openFile` of a segment data file reads the segment's
hotcache once (one small read, remembered per directory) and calls
`prefetchRanges` for that file with its ranges. A segment without a hotcache, or
with one that does not decode, opens with the same reads issued one after another;
the hotcache is an optimization and a damaged one is ignored (verify-on-open and
the scrub still check the file itself).

Measured on a collection of two 3.4 MB segments, six data files, 4 KiB chunks:
opening both segments takes 10 reads of segment files and fetches 34 KB, 1% of
the collection. `HotcacheSegmentTest` pins the per-segment bound.
The request count depends on the number of files, not on the size of the segment.

## The directory

`ObjectStoreDirectory` is a `Directory` over the objects that share a key prefix.

- **Capabilities** are `REMOTE | ATOMIC_RENAME | DURABLE_SYNC`: no hard links, and
  no stable mapped bytes.
- **Writes** go to a local staging directory (RAM by default, or a filesystem
  directory under `staging_dir`, one subdirectory per `Directory` instance, removed
  with it). `finishFile` closes the staged file and uploads it with one `put`. The
  staged copy stays readable until the file is deleted or the directory is
  destroyed, so a segment that was just written is not fetched back. `sync` has
  nothing to do: an acknowledged put is durable. With a staging directory,
  `spill_bytes` sets when a stream stops being folded into the segment's first
  file and becomes an object of its own (see below).
- **`renameFile`** is a server-side copy and a remove. The target object is
  replaced atomically, which is all `CURRENT` needs.
- **`linkFile`** is a server-side copy; between different stores it streams the
  bytes. `reuseFiles` copies, as the directory has no `HARD_LINK`.
- **`listFiles`** lists the prefix, hiding marker objects and other directories.
- **Known sizes.** `Manifest::load` passes the committed files of the manifest it
  chose to `Directory::knownFiles`. The directory keeps their sizes, so opening a
  committed file needs no size request, and a segment listed without an
  `s<seg>__hot` has no hotcache, so its open does not probe for one. A name is
  forgotten when the directory deletes, writes, renames or links over it.
  Reopening a small two-commit collection takes 27 requests instead of 31, and
  none of them asks for the size of a segment file.
- **`openChecksum`** streams `chunk_bytes` ranges through the hash without a
  mapping, so verify-on-open and the scrub read an object once and do not make it
  resident. `map()` returns a lazy file.
- **Deletes wait for readers.** On a filesystem a merged-away segment stays
  readable for the queries that still hold it, because the unlink leaves the
  inode. A lazy file would fail on its next fetch, so the directory counts the
  lazy files and the `openChecksum` handles it handed out, and a delete of an
  object a reader holds hides the name at once and removes the object when the
  last reader is destroyed. A handle is a reader until it and every file it
  mapped are gone, even past the directory, so a reader that secures its
  snapshot's files by handle maps them after a merge deleted them, as an
  unlinked descriptor does on a filesystem. A crash
  in between leaves an unreferenced object, which the startup sweep of unreferenced
  index files removes.
- Files that are not segment data (`CURRENT`, commit manifests, live docs,
  overlays) are read whole when opened: they are small, and unlike a segment file
  they can be replaced in place.

Files per segment: the writer folds small streams into file 0 (a stream that never
left RAM is "relocatable"), so a small segment is a single object, fetched whole
by the first query that needs anything but its catalog. Streams that spilled
(4 MiB by default; `spill_bytes` with a staging directory) stay separate and
survive as their own files, which is what a lazy reader can leave unfetched.
Choose `spill_bytes` for the trade between many small objects and coarse ones.

## The backend

`ObjectStoreDirFactory(std::shared_ptr<ObjectStore>, const StorageContext&)` lays
collections out as keys, the layout of the `fs` backend:

```
t/<tenant>/<collection>/CURRENT
t/<tenant>/<collection>/<incarnation>/<file>
node/<file>                                   (node-level files, replication.json)
```

`createCollection` is a conditional `put` (`ifAbsent`) of a marker object, so two
writers cannot both create a collection; a conflict is an exception whose
message contains "already exists". `collections()` and `incarnations()` use
`listDirectories`, which a store with a delimiter listing can answer cheaply and
which defaults to listing everything under the prefix.

`ram_object` is the built-in backend over a `RamObjectStore`: it is for tests and
demos, and the reference for what a plugin provides. A plugin registers its own
backend by constructing `ObjectStoreDirFactory` over its own `ObjectStore`; see
[plugins](../dev/plugins.md#object-stores). The backend is writer-owned like `fs`
is: a second writer on the same keys is not detected beyond the collection marker.

## What is deferred

- **Range-declared postings reads.** See above: a query fetches whole files.
- **Eviction.** Resident chunks stay until the file is destroyed, and the process
  holds an open file per segment file it has used. A byte budget with a clock over
  resident chunks fits the bitmap; it needs the readers to stop assuming a pointer
  stays valid, or readers to pin ranges.
- **Parallel fetches.** A `prefetchRanges` call fetches its runs one after another.
- **Multipart upload and write-behind.** `put` takes a whole object, which an S3
  plugin has to split itself, and a flush uploads from the staged file before it
  returns.
- **A tiered or cached layer.** The layer registry composes (a `DelegatingDirFactory`
  over an object store), but no layer ships.
