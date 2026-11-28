# Optional `_source_` store

A collection can keep every document exactly as it was submitted, in files that
are independent of the index format. This is for reindexing and recovery: any
collection, however damaged or old, can be rebuilt from its sources. The store
is **off by default**, declared as the reserved stored resource `_source_`, and
costs search nothing: no search or warmup reads a source file, and opening a
segment reads nothing from them. It takes a handle on the segment's source map
and on each source file its manifest entry lists, sized from the manifest. The
map is read on the first fetch, and a file's header and footer on the first
fetch that needs that file.

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
- Each flushed segment writes one **source file** `v<seq>.<epoch>` holding its
  documents (id, body, update version, flags) in compressed blocks, and a
  **source map** `s<seg>.<epoch>__S<gen>` that points the segment's doc IDs at
  `(file, ordinal)` runs. `seq` comes from `IndexInfo.next_source_seq`, which is
  durable only at commit, so a writer reopened after an uncommitted flush or merge
  hands the same `seq` out again; the writer epoch in the name keeps the name
  fresh, and the startup sweep removes the older epoch's orphan. The segment
  records the map's generation in `SegmentInfo.source_gen`; a source-only
  patch publishes a later one (see [Source-only patches](#source-only-patches)).
- Record *i* of a flush is doc *i*: `Inverter::addSource` pads documents indexed
  without a source with placeholders and throws if a call would put a record
  anywhere else (a second call for one doc, or one before `startDoc`).
- **Merges move pointers, not bytes.** The merged map is the inputs' runs over
  the same files, following the merge's own document mapping
  (`SegmentMerger::docMapping`), so a merge that reorders documents only produces
  more runs. A file is referenced as it is while at least `min_live_ratio`
  (default 0.5) of its documents is live, it is at least `min_file_bytes`
  (default 4 MiB), and it holds at least `1/max_files` (default 32) of the
  segment's live source bytes. Any other file has its live records copied into
  one new file (`mergeSources` over a `SourceRelocation`): full blocks
  compressed with the current codec and dictionary are copied without
  recompression, and partial blocks (a flush's last block) and blocks of an
  older codec or dictionary are repacked, so trickle ingest does not leave
  files of tiny blocks and old blocks move to the dictionary. A segment
  therefore references at most `max_files + 1` files (more only when a file
  the cap would relocate cannot be verified and stays referenced), and space amplification
  per referenced file is at most `1 / min_live_ratio`. When more than
  `max_files` files qualify, the ones with the most live bytes stay referenced,
  since they cost the most to copy.
- The layout code (`SourceMerge`) knows files, ordinals and runs; what a record
  holds is the source format's business, behind the relocation seam.
- **Switching the store off deletes existing sources.** With
  `"_source_": {"enabled": false}` a merge drops each input's map and references,
  so the space is reclaimed and those documents' sources are gone for good, even
  if the store is switched on again later. There is no pause that keeps them.
- A merged map that names a file none of its inputs lists fails the merge rather
  than produce a segment whose map points at a file retirement would delete.

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
- A source-only patch's file is held by reference by each segment whose map
  names it, and leaves a segment's list once a later patch replaced all of
  its records there; it is retired when no segment or snapshot lists it.
  After a reopen such a file is part of the segment's loaded file list and
  stays listed until a merge.
- `Manifest::indexFile` recognizes source file and map names, and
  `Manifest::fileEpoch` reads their epoch, so orphan sweeps reach them.

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
none. Version 2 keeps the envelope small: header fields are varints, checksums
are 32 bits (the header and payload checksums together give 64), footer entries
are delta-coded varints, and versions are zigzag deltas within a block, so a
block costs about 30 bytes of envelope plus a 2 to 4 byte footer entry rather
than 62.

A file carries the dictionary its blocks were compressed with as a dictionary
block. The id is derived from the dictionary bytes, so a block can be copied
verbatim into another file that carries the same dictionary. The store keeps one
dictionary (`StoredDictionaryRegistry`, slot `_source_`, the same epoch rules
as the inline resources). The first merge whose inputs hold at least the sample
budget of source trains it from bodies sampled across their blocks. Flushes and
relocations after that use it, and a restart takes the latest dictionary of the
largest live source file. With 16 KiB blocks a dictionary saves about as much as
quadrupling the block size, without the fetch cost of decompressing larger
blocks, so the block size stays 16 KiB. The format grows only through new codec ids, record flag bits and block
kinds; a reader skips block kinds it does not know. The parser refuses a block
whose claimed record count its payload cannot hold before allocating for it.

Record flags: minified JSON body, serialized `luxir.Map` body (gRPC and any
transport that kept no bytes), tombstone, `allow_dups`, "missing" (a
placeholder for a doc indexed without a source, which keeps ordinal == doc ID),
"filtered" (includes/excludes narrowed the body, so it is not the whole
document and a rebuild from it would be narrower than the original), and
"mapped" (a request `field_map` renamed keys, so the body's keys are not the
indexed field names).

## Stored fields served from source

A stored TEXT/STRING value would otherwise be written twice, once in stored
fields and once in the source body, which is up to half a document's bytes.
With the store on, a root-document field that is stored and not `stored_copy`
is *source-backed*: the stored-fields writer lists it in its resource metadata
and leaves it out of every document whose record holds the field values
(`source::holdsFieldValues`: a JSON or Map body that is neither filtered nor
mapped). A fetch that finds no stored value for a source-backed field reads it
from the record with `SourceReader::Fields`. It parses the body with the reader
ingest used and renders the last value of the field's key through
`storedValues`, the function ingest stores with, so the output is the same bytes
the stored copy would have been. Other documents store the field as before, so
a reader needs no per-document flag.

Merges carry the source-backed list into their output. A merge that drops the
sources (the store was turned off) instead reads each input's records and writes
the values into stored fields, so they stay retrievable. A source map that does
not load fails such a merge, which is retried. A record that cannot be read lost
its values with its source, as it would for `_source`.

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
before it is written, and nothing else: the document is indexed in full. A
narrowed record carries the "filtered" flag.
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

## Partial updates

`UpdateRequest.patches` ([indexing guide](../guide/indexing.md#partial-updates))
edit a document's record and index the result in place of the document. They
travel with their request's update version and are resolved by that
request's own commit, which has applied every earlier update:

1. **Resolve.** After the commit applies its deletes, each patched id's base
   is its newest live document born before the request's version, found with
   the id filter and a term seek in every segment of the commit's snapshot. A
   live document born later means a newer write already replaced it
   (`superseded`), or a newer unpublished patch result is the base (below).
   The base's record is fetched and rendered as JSON; a
   filtered, mapped, damaged or missing record fails the patch.
2. **Apply.** `source::PatchDoc` edits the JSON in place: it splits only the
   objects and arrays an operation descends into, so every untouched value
   keeps its bytes. Patches of one id in one request chain in order. A result
   equal to the base writes nothing.
3. **Index.** After the commit publishes, a follow-up message indexes each
   changed document through the ordinary ingest path (the record becomes its
   new source), with the request's update version as its birth and overwrite
   on, so its delete watermark retires the base like any overwrite. The
   follow-up's commit publishes them and completes the request.

Between the request's admission and that publication, the request's version
pins delete retirement: the delete frontier and every commit's retirement stop
below it. A newer overwrite or delete of a patched id, committed before the
patched document reaches a segment, would otherwise retire its packet and
leave the patched document live beside it; pinned, the packet stays pending
and reaches the patched document's segment. A commit that fails before it
publishes discards the resolution of the patches it would reindex, so they
had no effect; a source-only patch (below) is already attached to its segment
and is published by the next commit, like documents a failed commit flushed.

A message admitted while an earlier one's patched documents are resolved but
not yet indexed must build on them. The writer keeps those documents by id
(`unpublishedPatches`) until their follow-up completes; resolution takes such
a document as the base when it is newer than every segment document of the
id and no packet the commit applied deletes the id after it.

### Source-only patches

A field with no postings, column, stored copy or variant (a STRING-like field
with `index: none`, `column: false`, `stored: false`) leaves nothing in a
segment but its source: ingest only checks its shape. A patch whose result
differs from its base only in such fields (`source::changedKeys` compares the
last value of each top-level key byte for byte) needs no reindexing, so
resolution replaces the record instead (`IndexWriter::writeSourcePatches`):

1. The commit writes the new records of all such patches to one new source
   file, without a dictionary (a file carries its dictionary, which would
   outweigh a few records; the next merge relocates them into one that has it).
2. Each segment holding a patched document gets the next generation of its
   map: the document's run is split to point at its new record, and the map's
   version table (`SourceMap::versions`) records the patch's update version.
   Files no run refers to any more drop out of the map. Each commit that
   patches a segment adds a file to it, so when the map would name more than
   `2 * max_files` files, the records of the files it uses least are copied
   into the commit's file as well (a fold, read in file order and written in
   doc order so neighbouring docs join one run), down to `max_files`; they
   keep their versions. A map is rewritten whole, so a commit's cost grows
   with the runs and versions of each segment it patches; per-commit override
   files would make it proportional to the patches instead.
3. Under `indexMutex`, the new map and file are attached to the segment
   (`SegInfo::patchedSourceMap`, `patchedSourceFiles`; `baseFiles` stays
   immutable), its `sourceGen` advances, and the same commit publishes them:
   the manifest lists them with the segment's files and sets
   `SegmentInfo.source_versions`. A reader opens a new `SourceReader` when the
   map name changes. The replaced map goes once no snapshot lists it
   (`SegInfo::obsoleteSourceFiles`, retired with the obsolete live-docs
   generations after each commit).

The document keeps its birth, so delete watermarks treat it as before: any
later overwrite or delete of the id is newer than the patch and retires it
with the document. Its `_version_` is the recorded version: retrieval reads
the map's version table for segments with `source_versions`, and resolution
takes it as the base version, so `if_version` holds. A merge carries the
version table through its document mapping (`mergeSources`), and its
liveness accounting sees the replaced records as garbage, which relocation
reclaims under `min_live_ratio`.

A merge that has read a segment's map would not carry a newer generation into
its output, so a segment that is merging (or was merged into a segment not yet
published) is not patched this way: its documents take the reindexing path,
decided again under `indexMutex` when the map is installed. So is a document
whose base is an unpublished patch result. A failure writing the file or a
map, or reading a record to fold, also falls back to reindexing. Like documents a failed commit flushed, a map installed by a
commit that fails later is published by the next commit.

A source-only field is retrieved only when a request names it, by the same
path as a source-backed stored field (`loadSourceBackedValues`).

Writers start their update versions `2^32` past the manifest's when they reopen
an index, so a version acknowledged before a crash, or by a primary a
promoted follower never saw, is never handed out again: `if_version` compares
against document births, and a reused version could match a different state.

## Not yet implemented

Retraining the dictionary, source-only compaction of a segment, degraded-file statistics, the offline
`luxir-source-dump` tool and tombstone files (phases 2 and 3 of the plan in issue 97), and
column updates without reindexing. A flush buffers the segment's whole
compressed source file in memory until the segment flushes (RAM-accounted, so
the budget stays honest, but with the store on a segment's RAM is roughly its
index plus its compressed sources, which means earlier flushes and more merges);
streaming blocks to the file as they seal is the natural follow-up.
