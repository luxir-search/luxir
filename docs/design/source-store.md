# Optional `_source_` store

A collection can keep every document exactly as it was submitted, in files that
are independent of the index format. This is for reindexing and recovery: any
collection, however damaged or old, can be rebuilt from its sources. The store
is **off by default**, declared as the reserved stored resource `_source_`, and
costs search nothing: no search or warmup reads a source file, and opening a
segment reads only the segment's small source map and takes a handle on each
source file. A file's header and footer are parsed on the first fetch that
needs it.

**Status: experimental.** The file format is meant to freeze, but there is no
compatibility promise until `luxir-source-dump` reads it and golden files and a
fuzzer pin it down. The Map record body still uses the live `luxir.Map`
encoding, which needs a frozen copy of its own before that.

## Shape

- `_source_` is a `stored_resources` entry with the `separated` layout: its
  bytes live in files of their own that a merge references rather than copies.
  The layout is only available to `_source_` in this release; the point of
  declaring it as a resource is that another resource could adopt the layout
  later by configuration.
- Each flushed segment writes one **source file** `v<seq>` holding its documents
  (id, body, update version, flags) in compressed blocks, and a **source map**
  `s<seg>__S<gen>` that points the segment's doc IDs at `(file, ordinal)` runs.
  `seq` comes from `IndexInfo.next_source_seq`, which is never reused; the segment
  records the map's generation in `SegmentInfo.source_gen`.
- **Merges move pointers, not bytes.** The merged map is the inputs' runs over
  the same files, following the merge's own document mapping
  (`SegmentMerger::mapDocId`), so a merge that reorders documents only produces
  more runs. A file is referenced as it is while at least `min_live_ratio`
  (default 0.5) of its documents is live, it is at least `min_file_bytes`
  (default 4 MiB), and it holds at least `1/max_files` (default 32) of the
  segment's live source bytes. Any other file has its live records copied into
  one new file (`mergeSources` over a `SourceRelocation`): full blocks are
  copied without recompression and partial blocks (a flush's last block) are
  repacked, so trickle ingest does not leave files of tiny blocks. A segment
  therefore references at most `max_files + 1` files, and space amplification
  per referenced file is at most `1 / min_live_ratio`.
- The layout code (`SourceMerge`) knows files, ordinals and runs; what a record
  holds is the source format's business, behind the relocation seam.
- With the store switched off (`"_source_": {"enabled": false}`) a merge drops
  the map and the references, so the space is reclaimed.

## File lifetime

A source file belongs to no segment name, so deleting a dead segment by prefix
cannot remove a file a merged segment still references. The rule is the core's
and is not specific to sources: a file a segment lists that is not under the
segment's own name prefix is **held by reference** (`SegInfo::sharedFiles`) and
goes with the last segment that lists it.

- A committed file is retired by the next commit's set difference
  (`CommitSnapshotRegistry::obsoleteFiles`) once no segment of the new snapshot
  lists it; a merge output that inherits it keeps it alive by listing it.
- A file only never-committed segments held is retired when those segments die
  without passing it on (`IndexWriter::unreferencedSharedFilesLocked`).
- `Manifest::indexFile` recognizes `v*` and `t*` names (and their `.tmp`
  leftovers) so orphan sweeps reach them.

Source files are listed in the `files` of every segment that references them,
so commit pinning and replication cover them; a follower downloads each source
byte about once.

## Damage

- Source files and maps are skipped by startup verification and the scrubber,
  so damage costs only the documents in the affected blocks, never the
  collection. Replication still checks every downloaded file.
- Unreadable never means garbage. A merge whose input map cannot be read fails
  and is retried later, because without the map it cannot carry the segment's
  references. A source file that is missing, unreadable or has a damaged block
  among the live records is never relocated: the merge keeps referencing it as
  it is, and only the damaged documents stay unreadable.
- A fetch of a damaged document returns null; an export (`source=true`) fails
  the request instead, so `export | _update` never silently loses documents.

## File format

`src/luxir/source/SourceFormat.h` is the specification and the only code a
standalone reader needs. It depends on neither the schema nor the segment
format. Every block starts with a per-file sync marker and carries its own
checksums, seeded with the block offset, so one damaged block loses only its
documents and a scan can resynchronize; the footer is an accelerator that is
rebuilt by scanning when it does not verify. Codecs are zstd (default), lz4 and
none. The format grows only through new codec ids, record flag bits and block
kinds; a reader skips block kinds it does not know. The parser refuses a block
whose claimed record count its payload cannot hold before allocating for it.

Record flags: minified JSON body, serialized `luxir.Map` body (gRPC and any
transport that kept no bytes), tombstone, `allow_dups`, and "missing" (a
placeholder for a doc indexed without a source, which keeps ordinal == doc ID).

## Capture

The body is the document before `field_map` / `drop_unmapped`. NDJSON records,
the elements of a buffered `docs` array and the docs of an inline `_update_`
record are kept as the bytes that arrived, minus whitespace outside strings, so
key order, duplicate keys and number spelling survive. A transport retains those
bytes only for a collection whose schema (or, for a new collection, the node
default) stores sources; anything else is serialized as a `Map`, which is
value-exact rather than byte-exact. The same fallback covers a schema that turns
the store on between a request's parse and its admission; retaining every body
for every collection would cost memory when the store is off. The record id is
the document id as the index sees it (a numeric id renders canonically). The
version is the update version the writer assigned.

Source includes/excludes (`_source_.includes` / `excludes`, overridden per
request by `UpdateRequest.source_includes` / `source_excludes`) narrow the body
before it is written, and nothing else: the document is indexed in full.
`source::SourceFilter` filters the minified bytes without building a value tree
(kept values are copied byte for byte) and a `Map` by sharing its kept values.

## Settings

`stored_resources._source_`: `enabled`, `codec` (`zstd`, `lz4`), `zstd_level`,
`chunk_size_bytes`, `max_docs_per_chunk`, `min_live_ratio`, `min_file_bytes`,
`max_files`, `includes`, `excludes`. `mode=set` adds or replaces the entry;
`mode=replace_all` keeps the current entry unless the replacement names it,
because dropping it would make the next merges discard every stored source. A
new collection whose schema does not declare it takes
`--indexing.source-default` (default off); the entry is written into the
schema, so changing the node setting never alters an existing collection.
Enabling applies to documents indexed afterwards.

## Retrieval and export

`fields: ["_source"]` returns the document as raw JSON (never re-rendered). It is
not part of the default or wildcard projection; a document whose source is
missing or damaged comes back null. `_search?format=docs&source=true` with
`fields: ["_source"]` emits one stored document per line and fails the request
if any hit has no readable source, so `export | _update` is lossless. A source
that NDJSON ingest would read as a control record (`{}`, or a lone `_name_`
object) is emitted wrapped in an inline `_update_`.

## Cost when off

With no `_source_` declared, ingest, search and merges do no source work: a
merge returns before reading anything when no input has a source map, and
nothing else is reached without the schema flag. `test/bench/SourceBM.cpp`
measures the ingest and fetch costs when it is on.

## Not yet implemented

Trained zstd dictionaries (sharing the training code of the inline resources),
source-only compaction of a segment, degraded-file statistics, the offline
`luxir-source-dump` tool, tombstone files, source-only fields and partial updates
(phases 2 to 4 of the plan in issue 97).
