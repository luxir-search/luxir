# Optional `_source` store

A collection can keep every document exactly as it was submitted, in files that
are independent of the index format. This is for reindexing and recovery: any
collection, however damaged or old, can be rebuilt from its sources. The store
is **off by default** and costs search nothing: no search or warmup reads a
source file, and opening a segment only maps its map and files (reading nothing
from them). Mapping at open, not on first use, keeps a reader valid after a merge
retires a file and keeps it from holding the directory.

## Shape

- Each flushed segment writes one **source file** `v<seq>` holding its documents
  (id, body, update version, flags) in compressed blocks, and a **source map**
  `s<seg>__S<gen>` that points the segment's doc IDs at `(file, ordinal)` runs.
  `seq` comes from `IndexInfo.next_source_seq`, which is never reused; the segment
  records the map's generation in `SegmentInfo.source_gen`.
- Source files belong to no segment name, so deleting a dead segment by prefix
  cannot remove a file a merged segment still references. They are listed in the
  `files` of every segment that references them, so commit pinning and
  replication cover them, and a file goes when the last segment listing it does
  (`IndexWriter::unreferencedSourceFilesLocked`).
- **Merges move pointers, not bytes.** The merged map is the inputs' runs, cut at
  deleted docs, over the same files. A file whose live fraction falls below
  `min_live_ratio` (default 0.5), or that is smaller than `min_file_bytes`
  (default 4 MiB), has its live records copied into one new file instead
  (`mergeSources`); fully live blocks are copied without recompression. Space
  amplification per referenced file is at most `1 / min_live_ratio`. With the
  store switched off a merge drops the map and the references.
- Source files and maps are skipped by startup verification and the scrubber, so
  damage costs only the documents in the affected blocks, never the collection.
  Replication still checks every downloaded file.

## File format (frozen)

`src/luxir/source/SourceFormat.h` is the specification and the only code a
standalone reader needs. It depends on neither the schema nor the segment
format. Every block starts with a per-file sync marker and carries its own
checksums, seeded with the block offset, so one damaged block loses only its
documents and a scan can resynchronize; the footer is an accelerator that is
rebuilt by scanning when it does not verify. The format grows only through new
codec ids, record flag bits and block kinds; a reader skips block kinds it does
not know.

Record flags: minified JSON body, serialized `luxir.Map` body (gRPC and any
transport that kept no bytes), tombstone, `allow_dups`, and "missing" (a
placeholder for a doc indexed without a source, which keeps ordinal == doc ID).

## Capture

The body is the document before `field_map` / `drop_unmapped`. NDJSON records and
the elements of a buffered `docs` array are kept as the bytes that arrived,
minus whitespace outside strings, so key order, duplicate keys and number
spelling survive. A transport retains those bytes only for a collection whose
schema (or, for a new collection, the node default) stores sources; anything else
is serialized as a `Map`. The version is the update version the writer assigned.

## Settings

`SchemaDef.source`: `enabled`, `codec` (`zstd`, `none`), `zstd_level`,
`block_size_bytes`, `min_live_ratio`, `min_file_bytes`. A new collection whose
schema does not say takes `--indexing.source-default` (default off); the choice
is written into the schema, so changing the node setting never alters an
existing collection. Enabling applies to documents indexed afterwards.

## Retrieval and export

`fields: ["_source"]` returns the document as raw JSON (never re-rendered). It is
not part of the default or wildcard projection. `_search?format=docs&source=true`
with `fields: ["_source"]` emits one stored document per line, so
`export | _update` is lossless. A document whose source is missing or damaged
comes back null (an empty object in the export).

## Not yet implemented

Trained zstd dictionaries, source-only compaction of a segment, degraded-file
statistics, `exclude_vectors`, the offline `luxir-source-dump` tool, tombstone
files, source-only fields and partial updates (phases 2 to 4 of the plan in
issue 97).
