# Vector Search Internals

Design of the vector indexing and search path: how per-segment FAISS aux
indexes are recorded, built, and queried. The user-visible contract (query
knobs, scoring, recall) is in
[guide/vector-search.md](../guide/vector-search.md).

The stored vector column is the source of truth; ANN indexes are derived,
per-segment aux overlays behind an engine seam. Exact flat KNN, cosine
raw-column normalization, candidate rescoring, and creation of missing
overlays all read or regenerate from the column.

## Column Encoding

A vector field's column stores one fixed-size value per vector in the field's
`encoding`: `f32` (4 bytes per dimension), `f16` (2), or `int8` (8 + dims
bytes per vector). The encoding is recorded in each segment's field metadata
rather than read from the schema, so a schema change never reinterprets an
existing segment, and the column stays a plain fixed-size binary column
(`dims` is derived from the value size and the encoding).

`int8` follows the per-vector scaling of LVQ (Aguerrebere et al., "Similarity
search in the blink of an eye with compressed indices", VLDB 2023,
arXiv:2304.04759): each value is `offset, scale, codes[dims]` with
`x[i] ~= offset + scale * code[i]`, where offset is the vector's minimum and
scale is its range over 255. LVQ also centers the data on a dataset mean
before quantizing; Luxir does not, because any shared statistic would tie a
stored value to the segment it was written in. With purely per-vector
parameters:

- Each value is self-describing. Merges copy the encoded bytes verbatim, and
  never re-quantize already-quantized data.
- No per-segment or per-dimension statistics exist, so nothing has to be
  recomputed or reconciled when immutable segments merge, and a vector's
  bytes do not change when its neighbors are deleted.
- Quantization error is bounded by each vector's own range: half a step,
  `(max - min) / 510`, per element.

A merge of inputs with different encodings (a schema change between them)
writes the most precise input encoding and transcodes only the less precise
inputs, so a merge never loses precision; same-encoding merges are pure byte
copies.

### Norm prefix

Cosine needs `|x|` of every stored vector unless the column holds `f32` unit
vectors, and `int8` L2 needs `|x|^2` (below). Recomputing it costs a second
reduction over every row on every query, while it is a property of the stored
value, so the writer stores it once: the segment field metadata records
`vectorNorms`, and each row then starts with a 4-byte float `|x|^2` of the
vector the payload decodes to (for `int8`, of the exact `offset + scale *
code` reconstruction), followed by the encoding's payload. Fields that need it
(`vcodec::needsNorms`): cosine over `f16` or `int8`, cosine over raw `f32`
(`normalize_on_write: false`), and `int8` L2. At 768 dimensions that is 0.1%
of an `f32` row and 0.5% of an `int8` row.

A prefix in the row rather than a companion column (like `keep_original`'s):

- Rows stay self-describing, so a same-layout merge is still a byte copy and
  there is no second column to keep rank-aligned across merges and schema
  changes.
- The norm is in the first cache line the scorer touches for that row: a
  rescore, which gathers scattered rows, does no extra random read.
- `f32` payloads keep their 4-byte alignment relative to the row.

A merge keeps the prefix when any input already in the output encoding has
it, and computes it for the inputs that lack it. A segment written under
settings that did not need it (before a metric change, say) is still scored
correctly: the scorer computes the same value per row until a merge adds it.

### Scoring kernels

Exact scoring means exact over the stored encoding. `VectorScorer` is the
single place a float query is scored against a stored row, used by the flat
column engine and the terminal candidate rescore. `int8` rows are scored
without decoding to a float buffer, using per-query `sum(q)` and `|q|^2` and
the stored `|x|^2`:

```text
q.x   = offset * sum(q) + scale * dot(q, code)
|q-x|^2 = |q|^2 - 2 q.x + |x|^2
```

so the per-element work is a pure `dot(q, code)`. Cosine over `f16` and `int8`
always divides by the stored vector's norm, since a quantized unit vector is
not exactly unit length.

The per-element loops are SIMD kernels (`codec/VectorKernels.cpp`): one pass
over the stored bytes per row, with `f16` widened to float in registers
(F16C `vcvtph2ps` on x86, `fcvtl` on AArch64) and fused into the FMA, and
`uint8` codes zero-extended and converted in registers, with no conversion
pass into a buffer. The instruction set is chosen at compile time from the
build's CPU target (`LUXIR_CPU_TARGET`), like the project's other SIMD code:
AVX2 + FMA + F16C on `x86-64-v3` and above, NEON on AArch64, and a portable
fixed-width lane loop (which the compiler vectorizes for the baseline target)
otherwise.

The scorer is batched: the flat scan collects eligible ranks into blocks of
64 rows and scores each block in one call, and the rescore scores each
segment's rank-sorted candidate bucket in one call, prefetching rows ahead
when they are not adjacent. Dispatch on encoding and metric happens once per
block.

Floating-point sums are not associative, so determinism is designed in. Every
kernel uses one lane layout, accumulator count, tail handling (a zero-padded
final vector) and horizontal reduction order for a given `dims`, and the
kernels live in one translation unit so contraction into FMAs cannot differ
per call site. A row's score therefore depends only on the query and the row,
never on its block, task, or thread: parallel and serial runs stay
bit-identical, and a row scores the same in the scan and in the rescore.

The kernels are shaped for memory-level parallelism, since a column larger
than the cache makes the scan memory-bound: eight independent accumulators per
row and a software prefetch a fixed distance ahead, which runs into the next
row of a contiguous scan.

`int8` keeps the float query. An integer kernel (query quantized to `int8`,
`vpmaddubsw` / VNNI-style `u8 x s8` dot products) was measured against it on
50k x 768 clustered vectors: about 20% faster per scan in a microbenchmark,
but inner-product recall@10 against float32 ground truth fell from 0.976 to
0.892, because the query's rounding adds to every score on top of the
stored vectors' own. Exact search must not lose ranking quality for speed,
so the integer path is not used.

`keep_original` writes the submitted float32 vectors, before cosine
normalization, to a companion column named `<field>__#orig` (an invalid
variant label, so schema lookups and field discovery never resolve it). It is
written in lockstep with the encoded column and merged like any column, so the
two stay aligned rank for rank. The query path never reads it. Readers use it
only when its value and document counts equal the encoded column's: enabling
`keep_original` on an existing field leaves older segments without it, and a
merge that mixes them produces a partial companion, which is ignored.

## Overlay Lifecycle

Vector aux indexes are recorded as segment overlays in the commit
manifest. Each segment's `SegmentInfo` can carry `AuxIndexInfo` entries
whose vector kind is `vector_faiss` and whose opaque metadata is
`VectorAuxMeta`.

A vector aux build is introduced during commit when `build_aux_indexes`
selects a vector field or `"*"`. Plain commits without vector selectors never
build vector overlays. After a segment has a vector overlay, that overlay
follows the segment by liveness: later commits carry it forward without
checking the shard core generation. Delete-only commits keep existing
overlays; deleted-document vectors remain in FAISS and are filtered at query
time.

Explicit vector selectors also activate those concrete overlay names in the
writer. That activation is process-local unless it produced a durable overlay
entry. A commit that fails mid-build has no side effects: staged overlay files
are deleted, no overlay entries are published, and no names are activated -
the caller sees the error and decides whether to retry. Startup seeds active
names only from overlays already present in the commit manifest, so Luxir never treats
intent-only state as a boot-time "must build" queue.

Explicit commit-time vector builds stage overlay files and entries across the
whole selected commit. If any requested vector build fails, staged overlay
files are deleted and no partial vector overlay entries are retained for a
later plain commit to publish.

Rebuilding does not require document reindexing at the storage level, but the
current public API cannot remove an existing per-segment overlay. The writer
has an internal drop/rebuild seam; exposing a safe collection-level rebuild is
future work.

## Build Policy

Build policy is per segment and size based. IVF+PQ is attempted only when the
segment has enough vectors to satisfy the training floor and enough
`N * dims` scan cost to justify an ANN index. Segments below either threshold
stay on the flat-over-column path. This mixed composition is normal: a query
can use FAISS for large segments and exact column scan for small segments.

The IVF+PQ training set is a strided sample across the segment's vector
values, not a prefix sample. This avoids training only on the oldest values
when data inside a segment is time ordered. The builder reads the
`keep_original` column when it is present and aligned, else decodes the
encoded column, and normalizes vectors for FAISS when the field uses cosine
and its input is raw (a raw column, the original column, or a quantized
column).

## Memory-Mapped Index Layout

Index data stays memory-mapped at query time. The aux file holds a small FAISS
header (index parameters, IVF centroids, PQ codebooks) followed by the
inverted-list payloads (PQ codes and vector ids) in Luxir's own layout; the
reader decodes only the header into memory and serves list scans directly from
the mmapped file. Residency of the bulk index data is therefore managed by the
OS page cache, like the vector column itself, rather than forced into process
RAM.

## Merging

Merges drop overlays for merged-away segments. The merged segment is treated
as a new segment and gets fresh overlays inside the merge-private phase for
active vector fields that pass the same thresholds. Whichever later commit
publishes the merged segment publishes its overlay entries atomically with the
segment. If a merge-time vector overlay build fails, Luxir deletes the staged
overlay files, publishes the merged segment without vector overlays, and
serves that field through the exact flat column fallback until a later
explicit build succeeds.

## Query Execution

`KnnQuery` creates one vector engine per segment. A segment uses its
`vec.<field>` FAISS overlay when present, or scans the vector column exactly
when no overlay exists. FAISS ids are segment-local vector value ranks, so the
query path maps each returned value rank back through that segment's vector
column metadata before liveDocs, filters, scoring, and multi-valued collapse.

IndexReader opens and pins overlay files eagerly when it opens an index
version, so cleanup can unlink old files without breaking existing readers.
The FAISS index bytes are decoded lazily on first kNN use and then cached on
the segment reader.

Internally, an `ivf.nprobe` request becomes a scan fraction
`nprobe / sqrt(live_vector_count)` applied across the current per-segment
indexes; that is what makes the knob merge-stable. Engine-specific knobs are
grouped under a per-engine sub-message on the wire (`ivf` today) so a second
engine adds a sibling with its own units instead of overloading these; the
host-side knobs (`k`, `refine_candidates`, `exact`) stay at the top level.

For per-segment IVF, Luxir ranks all segments' IVF lists by query-to-centroid
distance, then probes the globally best lists until the requested scan
fraction is reached. List cost is based on that list's live vector count
divided by the field's live vector count. This handles uneven list sizes and
avoids applying equal work to every segment. For an IVF segment with deletions
the live count is exact: it rides along with the segment's cached
rank-liveness bitmap, which is rebuilt per delete generation in time
proportional to the deleted docs (for a dense single-valued field the bitmap
simply borrows the liveDocs bits). Flat-scanned segments with deletions use a
cheap upper bound instead (exact counting there would scan the field per
query); the bound only errs toward slightly leaner initial effort, which
auto-deepening recovers.

Probed lists are always scanned in full. Entries within an IVF list are stored
in insertion order, not relevance order, so a partial list scan would drop
arbitrary candidates; `nprobe` (which selects lists by relevance) is the only
work limiter.

Candidates are unioned across deepen rounds using approximate scores for the
widen decision. From the first deepen round on, already-pooled vectors are
excluded from every engine's scan: the pooled set is folded into per-query
copies of the segments' liveness bitmaps (all-ones where a segment has no
deletions) and handed to the scanners as an eligibility filter, so each
round's result heap is spent entirely on new candidates and each round
requests only the projected shortfall rather than re-requesting the whole
pool. The shared per-segment liveness cache itself is never modified by
queries. Once widening finishes, the final candidate pool is rescanned from
the stored column (exact over its encoding), sorted by exact score, and
collapsed to one hit per document.

## Parallel Execution

A single query's scan and rescore run in parallel when the request executes in
parallel mode. The selected IVF lists are split into contiguous per-segment
chunks sized by their actual vector counts, and each chunk (plus each
column-scanned segment) is scanned as an independent task; the terminal
column rescore runs one task per segment whose candidate bucket
exceeds a small grain (smaller buckets, including the common small-`k`
single-segment pool, fold inline on the calling thread). Task boundaries do
not depend on the execution mode, and every bounded candidate cut uses a total
order, so a parallel run returns bit-identical results to a serial run of the
same query.

## What This Means for Users

- Bulk ANN index data is paged by the OS rather than loaded into process
  RAM, so indexes larger than RAM still work.
- Because the column is the source of truth, ANN indexes can be rebuilt or
  retuned per segment without reindexing any documents.
- Segments below the training threshold, and segments whose build failed,
  are searched exactly from the column. A failed build leaves no partial
  state.
- Parallelism is purely a speed knob: results are bit-identical to serial
  execution.
