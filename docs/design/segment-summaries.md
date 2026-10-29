# Segment summaries

Status: the per-segment sidecar is implemented; query-time pruning over it is
not yet. Related: [storage design](storage.md) for lazy segment opening and
the object-store direction.

## What a summary is

Every segment carries a small immutable summary of what its fields hold,
written with the segment at flush and at merge. It is the metadata a query
needs to prove that a segment cannot match, so that later work can skip the
segment without opening its data files. This layer writes and reads the
summaries; nothing consults them for matching yet.

The summary has one record per physical field of the segment's field
directory, stored-field resources (`_stored_` and named families) included,
so a complete summary proves that a field without a record holds no document
in the segment. Per field it records:

| Content | Fields |
| --- | --- |
| Identity | `fieldname`, persisted `type`, FieldType `flags` (segment markers included, such as `MAY_HAVE_FITTED`) and `type_flags` (for example a date's unit) |
| Counts | `docs_with_field`, `num_values` for column fields, BM25 header totals `sum_doc_freq` and `sum_total_term_freq`, distinct indexed terms `n_terms` |
| Numeric bounds | min and max in the column's encoded int64 space (INT raw, DATE in its own unit, FLOAT and DOUBLE as sortable bits) |
| Geo box | min and max encoded latitude and longitude of a `GEO_POINT` column; never wraps the antimeridian |
| Term bounds | the first and last indexed term, cut to 32 bytes |

At segment level it records `seg_id`, `max_doc` and `complete`.

The record's `summary_flags` say which parts are valid: `HAS_NUM_VALUES`,
`MULTI_VALUED`, `EMPTY` (the record exists but nothing was written for the
field), `HAS_NUMERIC`, `HAS_GEO_BOX`, `HAS_TERM_BOUNDS`,
`TERM_BOUNDS_TRUNCATED` and `TERM_MAX_UNBOUNDED`. A part that is not marked
valid proves nothing.

## Semantics

Every bound is conservative: it may be wider than the field's live values,
never narrower.

- Deleted documents are counted. A flush counts documents that failed or were
  deleted while indexing; a merge counts the documents it wrote. Delete-only
  commits never touch a summary, so deletes can only leave a bound wider than
  necessary. A merge recomputes everything from what it writes, so bounds
  tighten again.
- Numeric bounds are inclusive at both ends and cover every value of a
  multi-valued field, including the int64 extremes.
- Term bounds longer than 32 bytes are cut. The lower bound is a prefix of
  the first term, so it sorts at or below it. The upper bound is rounded up to
  the smallest string above every term that starts with the cut prefix
  (trailing 0xFF bytes stripped, the last byte incremented). When the cut
  prefix is all 0xFF there is no such string, and `TERM_MAX_UNBOUNDED` makes
  the upper bound infinite. Term bounds are over the fitted bytes the
  dictionary stores, so a query compares the bytes it would look up.
- A segment without a readable summary has none (`Segment::summary()` is
  null), and that proves nothing about it. The same holds for a summary that
  is not `complete`.

`SegmentSummary` provides the bound checks these rules imply
(`boundsExcludeTerm`, `boundsExcludePrefix`, `boundsExcludeTermRange`,
`mayIntersectNumeric`, `mayIntersectGeo`). They answer NO only as a proof.

## Storage

The summary is a segment overlay (`SegmentInfo.overlays`) of kind
`segment_summary`, name `summary`, at a fixed generation 0, in the file
`s<seg>__summary_00_00`. It takes no overlay generation, so the numbering of
vector overlays is unchanged. As an overlay it gets the existing manifest file
descriptor, sync, verify-on-open, scrub, replication, reopen reuse and
retirement with its segment, and it is listed with its size by
`_stats?segments=true`.

The file is header first and readable as one object:

```text
[0, 8)    "LUXIRSUM"
[8, 12)   u32 format version (1), little-endian
[12, end) encoded luxir.index.SegmentSummaryInfo, records sorted by name
```

There is one version, the file's. Field records are protobuf
(`FieldSummary` in `protos/luxir_index.proto`), so new fields can be added
without a new format.

`PostingsWriter::finish` records every field of the directory, with the
counts and totals taken from the directory itself and the bounds that the
field writers fed into the field's `FieldSummaryCollector`: the numeric column
writer its column's min and max, geo columns each point, and the term
dictionary (or the stitch of a parallel term-range merge) its first and last
term. `IndexWriter` writes the sidecar right after the segment's data files.
A failure to write it is logged and leaves the segment without a summary;
it never fails the flush or merge. A flush that produces no segment (only
deletes) writes no summary. `IndexReader` loads the summary when the
snapshot opens, reusing the previous snapshot's copy when the segment's files
and the summary's descriptor are unchanged. A summary file that is missing is
retried like any segment file; one that stays missing, cannot be decoded, or
names a different seg_id or max_doc than its segment is ignored, leaving the
segment without a summary.

### Why a sidecar and not the field catalog

[storage.md](storage.md) places per-segment bounds in the segment's field
catalog (`SegFieldInfo`, at the tail of the segment's first file) and a
collection summary in the manifest. This design instead keeps the mandatory
pruning metadata in a dedicated per-segment sidecar overlay:

- It is small: the counts and bounds of each field, without the data
  locations the catalog needs for reading.
- It can be cached on its own, for many cold segments, without holding their
  catalogs or data.
- It can be read without the segment's data file: one small object per
  segment, which suits a node reading from an object store.
- As an overlay it can be added later for an existing segment that lacks one,
  without rewriting the segment. No backfill path exists yet; such a segment
  simply has no summary.

The sidecar does not replace the catalog. The catalog stays the source of
data locations and of everything a reader needs once a segment is opened,
and some facts appear in both (type, flags, counts, BM25 totals). Both are
written once with the segment from the same field directory, so they agree.

## Deferred

- Query-time pruning: `mayMatch` rules over a summary view, one root mask per
  operation, BM25 statistics over every segment whether pruned or not. The
  bound checks above are what those rules build on.
- Lazy segment opening, so that a pruned segment's data is never mapped.
- A collection-level aggregate in the manifest, under a size budget.
- Membership structures (exact term sets, term and prefix filters). They help
  most on cold segments, where they are much smaller than the dictionary and
  can stay cached, but they cost build memory and space. They will come as a
  later, opt-in overlay kind per field, built for segments that have stopped
  merging, not bundled with this mandatory metadata. An exact set is not
  needed by default: a field small enough for one has a dictionary about as
  small.
- Vector statistics (centroids, boxes), to be designed together with the kNN
  segment ordering that would use them.
