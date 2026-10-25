# Segment summaries

Status: the per-segment sidecar and query-time pruning with one root mask per
operation are implemented; segments still open eagerly. Related: [storage design](storage.md) for lazy segment opening and
the object-store direction.

## What a summary is

Every segment carries a small immutable summary of what its fields hold,
written with the segment at flush and at merge. It is the metadata a query
needs to prove that a segment cannot match, so that later work can skip the
segment without opening its data files. Queries consult them to prune
segments per operation (see [Pruning](#pruning)).

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
  null), and that proves nothing about it. A summary that is not `complete`
  proves no absence: a field without a record may still hold documents. The
  records it does carry are as valid as in a complete summary, so their
  `EMPTY` flag and bounds still prove.

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

## Pruning

### Evidence, bottom-up

`Query::mayMatch(const SummaryView&)` answers, from one segment's summary
alone, whether the logical query can match a document there: `EMPTY` is a
proof that none does (over live and deleted documents alike), `UNKNOWN` is
always legal, `NONEMPTY` says only that no proof exists. It is a pure
function of the logical tree and never reads segment data. Leaves prove
absence from presence and bounds (`SummaryPredicates.h`):

| Query | Proof of absence |
| --- | --- |
| term, term set, phrase, combined fields | field absent or empty; the term outside the term bounds (a phrase needs every term, a set or combined-field query every entry proven absent) |
| prefix, term range | field absent or empty; the prefix or range outside the term bounds |
| fuzzy and other multi-term expansions | field absent or empty |
| numeric range or value set | field absent or empty; no admitted value inside the numeric bounds |
| exists | field absent, empty or without documents |
| geo box, geo distance | field absent or empty; the box (the circle's bounding box) disjoint from the geo box |
| match-no-docs | always |
| match-all | never (`NONEMPTY`) |

Composites combine their children: one `EMPTY` required or filter clause
empties a boolean; an optional group needs `min_match` (or, without required
clauses, one) members that are not `EMPTY`; prohibited clauses prove
nothing; a disjunction max needs every disjunct proven absent; boost,
constant-score, min-score, rescore and force-prepare wrappers pass their
child's answer through. A boolean counts duplicate optional clauses once,
against the `min_match` its logical plan executes after removing them (one
shared rule), so a proof never counts a clause the plan dropped. Every other query (kNN among them) answers
`UNKNOWN`. Without membership structures, a term inside a segment's term
bounds is never proven absent.

`QueryPruning` evaluates the trees an op is masked by (its query, each
filter, each domain variant's reset and extra queries) over every segment,
and memoizes the verdict per root query object for the request. One
evaluation visits each node of the tree once per segment; a leaf object
shared by two root queries is evaluated under each. `viable(query)` also
drops segments without live documents; `membership(query)` keeps them, as
it describes the query's matches before deletes.
`QueryPruning::disableForTests` turns every proof off, for tests and
benchmarks that compare pruned with unpruned execution; there is no request
flag.

### One root mask per op

The parser computes each query-consuming op's root mask from its own query
and filters:

- TopDocs: its query and every filter for its own matches, widened by every
  domain variant it produces for its children, so a child that resets its
  domain (`domain.query`) or has a filter routed away from it
  (`except_ops`) keeps the segments its own domain may match. A Fusion
  source also narrows by the fusion's shared filters, and the Fusion op needs
  the segments some source may match.
- The request root: per domain variant (a root-level op with its own
  `domain.query`), the segments its reset query and domain filters may match.

Masks are enforced at the op level, before domain and live-docs
construction. The root hands a child the empty domain (`DomainHandle::empty`)
where the child's variant or the child itself is pruned, and a segment no
child needs gets no live docs and no variant production. A TopDocs op skips a
pruned segment entirely: no supplier, scorer, filter, count or exact domain
is built; every child receives the empty domain; the collector still records
the segment as visited. A pruned segment therefore contributes exactly what
an empty match would: zero hits and counts, unchanged segment ordinals and
doc bases.

Pruning never changes what whole-index preparation sees. A prepared weight
chooses across every segment: kNN picks a global top k with the domain as
eligibility, and a required sibling of the kNN clause only post-filters
those k, so feeding it the empty domain where the sibling is proven absent
would change which documents it picks in other segments. Reader-stable
cache entries also require the canonical live domain on every segment. So
every PrepareContext keeps its unpruned domains: TopDocs input and effective
domains, a Fusion's shared filters, and, while any weight of the request
prepares over the whole index (`SearchOp::needsWholePreparation`), the
root's domains and variants on every segment. Pruning then only skips each
op's per-segment collection.

A cache-backed whole-membership plan records an empty cached membership for
a pruned segment only where the key query itself is proven empty there
(`membership`, deletes ignored). A segment pruned only by filters, a
Fusion's shared filters or the lack of live documents is skipped without
publishing anything, as the key query alone may match there.

Ops that consume no query of their own (facets, expression statistics)
are not masked: they count whatever domain their parent gives them, which is
empty on a pruned segment. Query facet buckets and the domain variants of
facet buckets are not pruned yet.

### Scoring statistics cover every segment

BM25 statistics never depend on pruning. Field totals (`docsWithField`,
`sumTotalTermFreq`) come from segment metadata for every segment of the
reader. Term statistics (term, phrase and combined-field weights, including
the case-insensitive rewrite) seek every segment as before, with one
exception: a segment whose summary proves the term itself absent (field
absent or empty, term outside the term bounds) is not seeked, because its
document and term frequency for the term are exactly zero. A segment pruned
by any other predicate (a sibling clause, a filter) is still seeked for
statistics, but never gets postings or a scorer. Scores are therefore
identical with and without pruning.

### Observability

Only when an op prunes a segment: explain notes
(`op <name>: pruned segment <seg_id> (ord <n>): <reason>`) and, under
`profile`, a `pruning` profile op with one piece per segment (`strategy`
`pruned` or `viable`, the reason in `details`). Proofs are not counted;
counters of actual segment opens and bytes come with lazy opening.

## Deferred

- Per-node enforcement: masks for nested clauses, query facet buckets and
  bucket domains, so a subtree skips segments its own proof excludes even
  when the op's root mask keeps them. It pays where opening is expensive.
- Matching-only statistics (one population per search), which would also
  skip dictionary reads in pruned segments.
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
