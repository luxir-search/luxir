# Segment summaries and pruning

Status: implemented. Related: [storage design](storage.md) for lazy segment
opening and the object-store direction, and the [schema guide](../guide/schema.md#segment-summaries)
for the per-field `summary` option.

## What a summary is

Every segment carries a small immutable summary of what its fields hold,
written with the segment at flush and merge. A query is evaluated against the
summaries first and a segment that provably cannot match is skipped without
opening its data files. The commit manifest also carries the union of all
segment summaries, so a caller can ask whether a whole collection may match a
query from the manifest alone.

Per physical field a summary records:

| Content | Fields | Proves |
| --- | --- | --- |
| Presence | every indexed or column field has a record | a field without a record holds no document |
| Counts | `docs_with_field`, `num_values`, BM25 header totals (`sum_doc_freq`, `sum_total_term_freq`, `n_terms`) | an `exists` query on an empty field matches nothing; scoring statistics without opening the segment |
| Numeric bounds | min and max in the column's encoded int64 space (INT raw, DATE epoch millis, FLOAT/DOUBLE sortable bits) | a range or exact value outside them matches nothing |
| Geo box | min and max encoded latitude and longitude, never wrapping | a box or circle outside it matches nothing |
| Term bounds | first and last indexed term, truncated to 32 bytes; the upper bound is rounded up so it stays a bound | a term, prefix or term range outside them matches nothing |
| Term presence | an exact sorted set (small sets) or a Binary Fuse 8 filter (large sets); a Bloom filter when the fuse filter exceeds its budget | a term the set lacks, or the filter reports absent, matches nothing |
| Prefix filters | optional fuse filters over the first L bytes of every term, per configured L | a prefix of length at least L the filter reports absent matches nothing |
| Vector statistics | fp32 centroid, per-dimension box and a radius that bounds every vector's distance to the centroid | nothing yet; collected for a later kNN segment ordering |

Every bound is conservative. A flush counts documents that failed or were
deleted during indexing; a merge counts the documents live when it started;
delete-only commits never touch summaries. Deleted documents can therefore
only widen a bound, never make a proof wrong. Merges recompute everything from
what they write, so bounds tighten again after a merge.

## Storage

The summary is a per-segment overlay (`SegmentInfo.overlays`, kind
`segment_summary`, file `s<seg>__summary_<gen>_00`). As an overlay it gets the
existing file ownership, sync, verify-on-open, scrub, replication, reopen
reuse and retirement for free, and nothing about it is rewritten when a later
commit only changes deletes or other overlays. The file is header first:

```text
"LUXIRSM1"  u32 infoLen  u32 indexLen
section 1: encoded SegmentSummaryInfo (field records sorted by name, payload directory)
section 2: payloads, 8-aligned (exact sets, filters, prefix filters, vector statistics)
```

A reader that fetches ranges needs the compact section 1 first and a payload
only when a filter tier is consulted; a local reader maps the file. Record
offsets are relative to the payload area so section 1 encodes independently of
its own size.

The manifest root carries `IndexInfo.field_summaries`: one record per physical
field over all segments, with the counts summed, the bounds unioned and no
payloads, plus `summary_complete`, which is false when any segment lacked a
readable summary. The aggregate is recomputed only when the segment set
changes (`core_gen`); delete-only commits copy it.

## Term filters

Binary Fuse 8 (Graf and Lemire, 2022) is the primary filter: about 9 bits per
distinct term at a 1/256 false positive rate, three probes in three adjacent
segments of the array (one small range read remotely), built once from the
complete key set. Keys are `XXH3_64bits` of the exact fitted term bytes the
dictionary stores (`summary::termKey`), versioned by `hash_version`; a reader
that sees another version treats every filter as unknown.

The builder keeps up to `filter_max_bytes * 8 / 9` keys while a field's terms
stream in. Past that the keys fold into a classic Bloom filter of the same
budget and the key buffer is released, so memory per field stays bounded by
roughly the budget plus 8 bytes per key. A set of at most `set_max` terms is
stored exactly instead of filtered. Merges rebuild filters from the merged
term stream; a parallel term-range merge collects keys per range and
concatenates them, since ranges are disjoint and ordered.

Rejected alternatives: Ribbon filters save another 10 percent of space at
considerably more code and build CPU; blocked Bloom filters buy cache-line
locality for 1.5 to 2 extra bits per key, the wrong trade when the fallback
case is the one short of bits. A filter over only rare terms (by document
frequency) is unsound unless frequent terms are stored exactly elsewhere, and
saves little space in Zipfian text; its real benefit, skipping sidecar probes
for common terms, belongs to a collection-level frequent-term set, left for
later (payload kind 6 is reserved).

## Pruning at query time

`Query::mayMatch(const SummaryView&)` is a pure function of the logical query
tree and one summary (a segment's or the collection aggregate). It answers
EMPTY only as a proof; UNKNOWN is always legal. Leaves consult evidence by
tier, cheapest first, and only as far as the view allows: live-document count,
field presence, bounds, exact sets, filters. Boolean combination: one
impossible required or filter clause empties the conjunction; an optional
group needs at least `min_match` (or one, when nothing else is required)
possible clauses; prohibited clauses prove nothing; kNN is unknown.

Every `Weight` computes the mask of segments it may execute against: its
parent's mask intersected with its own query's proofs, memoized per logical
subtree in the planning context. Children of a Boolean, constant-score,
minimum-score, rescore or force-prepare weight inherit the parent's mask. The
mask is enforced in the non-virtual entry points every consumer uses:
`scorerSupplier` returns null, `countShortcut` returns an exact zero,
`sparseFilteredTopKCost` returns zero and `prepare` hands prepared weights an
empty domain for pruned ordinals, so a pruned segment contributes nothing and
is never asked for a reader. Ordinals and document bases never change; every
operation still receives one slot per segment.

Statistics follow the mask: a weight's BM25 term and field statistics are sums
over its viable segments only, read from summaries where a segment is not
open. With nothing pruned they equal the whole-index totals exactly. This
keeps pruned segments closed at the price of scores that depend on the
segment layout; the `global_stats` request flag
(`PlanningContext::statsMask`) sums the statistics over every segment instead,
seeking terms in pruned segments too, while matching still follows the mask. A
reader-wide cached filter value (the kNN lane) is only built or read by a
weight whose mask covers every segment.

Lazy opening makes the proofs worth having: a snapshot opens with every
segment's summary loaded and its data files, live docs and overlays closed,
and a cell opens them on first use. Readers hold their snapshot's files
through the commit registry so a lazy open never meets a retired file.
Follower installs still open and verify everything eagerly. Known limits: a
string facet's global ordinal map still opens every segment whose summary
does not prove the field absent, and the field catalogs behind wildcard
retrieval open every segment once per core generation.

## Observability

- `profile: true` adds a `pruning` op with one piece per segment: strategy
  `pruned` or `viable`, with the proof as a detail.
- `?explain=resolved` lists `pruned segment <id> (ord n): <reason>` notes.
- Node counters `segments_considered` and `segments_pruned`
  (`luxir_search_segments_total{outcome}` in Prometheus).
- `_stats?segments=true` shows the summary overlay with its size per segment.

## Collection-level can-match

`POST /collections/{c}/_can_match` (and `luxir.Searcher/CanMatch`) takes a
search body, lowers each query-bearing op like a search would, and evaluates
it against the collection aggregate of the current snapshot. It opens no
segment. The answer names the snapshot (`commit`), whether the aggregate was
complete, and per op whether it may match and what proved it cannot;
`?segments=true` (or `profile: true` over gRPC) also lists the segment ids
nothing can match. A router over time- or tenant-partitioned collections asks
this before fanning a search out; a `NO_MATCH` is a proof for that snapshot
only, since a later commit can add matching documents.
