# Nested Documents

Owned sub-objects (variants, chunks, answers) are stored in per-path row
spaces inside each segment. This note records the layout, the read and write
paths, and how queries and operations change scope. The user contract is in
[the guide](../guide/nested.md); the direction and its rationale are in
issue #263.

## A scope is a schema and a reader

Each declared nested path `P` gets, in every segment, its own complete row
space: element rows written by their own `PostingsWriter` and read by an
ordinary `PostingsReader`, with the usual postings, columns, points, stored
fields and vectors. A child `Schema` (compiled from `P`'s `fields`, short
physical names, no reserved fields) and a child `IndexReader` over those
readers form a scope. The child reader's segments share `ord` with the root
reader's.

Everything written against (Schema, IndexReader) works in a scope without
change: query lowering and execution, BM25 statistics, kNN, facets and
metrics, emission. The root row space is exactly what a segment was before,
so root-only queries pay nothing (the "block tax" measured in #263 does not
apply).

```
A: variants [{price:10}, {price:12}]
B: variants []
C: variants [{price:7}]

root rows            variants rows
0  A                 0  price 10   (owner 0)
1  B                 1  price 12   (owner 0)
2  C                 2  price 7    (owner 2)

owner ends:  [2, 2, 3]      element owners: [0, 0, 2]
```

## Format

- Scope files use the segment-overlay file grammar with the overlay name
  `n.<path>`: `s<seg>.<epoch>__n.<path>_1.<epoch>_<fnum>`. They sort under
  the segment's prefix, so the dead-segment sweeps, `Manifest::indexFile` and
  `Manifest::fileEpoch` treat them like any other segment file, and they are
  listed in the segment's manifest `files` (replication and scrub are
  unchanged).
- A root segment's segment info lists the paths it carries. A segment
  written before a path existed carries none, and its element reader is
  empty (`PostingsReader::emptyScope`).
- A scope segment's segment info holds the owner map: one cumulative element
  end per owner row (the `endValueRank` monotone shape) and one owner row per
  element row (the `valDoc` shape, for candidates in random order such as
  ANN). `OwnerMap` reads both.
- Format version 13.

## Write path

- The root `Inverter` owns one child `Inverter` per path in use
  (`Inverter::scopeRows`). A nested input field (`NestedFieldType`) sends
  each element to the child: `startDoc()`, then the element's keys through
  the child schema's input handlers, with the same validation.
- The owner records its element end after the document. Element rows are
  append-only like owner rows: when a document fails or is rolled back, the
  owner is deleted as before, and its elements are dead because their owner
  is. There are no element tombstones, ids or deletes.
- The stored list is kept on the owner as one encoded `Val` in the path's
  stored resource, which retrieval decodes.
- Flush writes each child inverter into its scope segment, plus the owner
  map. Child row counts take part in `shouldFlush` and RAM accounting.

## Merge

Merge order is chosen once for owners. For each path, element liveness is
folded from the same owner live-docs snapshot the owner merge uses
(`LiveDocs::foldScope`), and an unmodified `SegmentMerger` runs over the
scope segments: surviving element runs stay consecutive and in owner order.
The merged owner map is rebuilt from the surviving owners' run lengths.
Merge admission also bounds each path's element rows by `MAX_SEGMENT_DOCS`.

## Read path

- `PostingsReader::openScopes` opens a root segment's scope segments once;
  every holder of the root reader shares them, so reopen reuses them.
- `IndexReader::scope(P)` builds the child reader lazily from the root core
  (`PhysicalCore::scopeCore`), shared by readers of the same core. Element
  liveness is folded from owner liveness and cached on the owner `LiveDocs`
  generation; a segment without dead elements has no element live set, so
  the null fast paths stay.
- The child reader has no shared filter cache. Element keys use short names,
  so they must not share a cache or a request use registry with owner keys.

## Changing scope in queries

`ToParentQuery` (`to_parent`) is lowered with a child `ParseContext` (child
schema and reader). Planning and weights use `PlanningContext::scope(P)` and
`Context::scope(P)`, which carry the request's pool, limits, warnings,
memory tracker and profiles but own their memos and filter-use registry.

- Scoring streams: element matches arrive in owner order, so the scorer
  reduces each run onto its owner with no structure sized by rows. With
  `max` and no count bounds the collector floor passes to the element scorer
  (an owner cannot beat its best element). Counts disable that pruning.
- Prepare (kNN and other domain consumers) gets the owner domain descended
  to the live elements of eligible owners (`descendDomain`), so filters
  apply before candidate selection.
- The owner filter key is the path, the count bounds and the element query's
  key; membership is cached in the owner space.
- `unit: "owner"` and `"owner": "root"` in collapse or diversify use a
  structural owner key (`CollapsePlan::addOwnerKey`): owner segment base plus
  owner row, read from the owner map.

## Changing scope in operations

`Domain.to_children` and `Domain.to_parent` wrap the operation in a
`ScopeOp`. It gathers the incoming domain of every segment, descends or
projects it, and runs the operation (with its whole subtree) as a
`NestedScopeRequest` over the destination reader and schema, serially and
into one final response that its result slot references. The request
carries the owner request's time zone, flags and formats. No owner-scope
operation path changed to support this.

`TopDocs.row_ops` runs one `NestedScopeRequest` per returned row with that
row's element range as its domain. `from_match` uses the named `to_parent`
clause's element query as the row query (or as a domain filter when the row
operation has its own query). Results are extra columns, so they are
row-aligned in every document format.

## Value position

From an owner, `P.f` in an expression binds to the element column of the
scope segment. An owner's element rows are contiguous, so their values are
one contiguous value-rank range, and the existing array reducers apply. In
`fields`, `P` and `P.f` read the owner's stored list; in element scope,
`root.f` reads the owner's stored value or column.

## Not in this design yet

- Element addressing and partial updates of element columns (#168).
- More than one nesting level at the API (the format allows it).
- ANN overlays for element vector fields (exact kNN is used in scope).
- Caching element-scope filters across requests.
- Restricting array reducers to matching elements.
