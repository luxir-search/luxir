# Vector Search Internals

Design of the vector indexing and search path: how per-segment FAISS aux
indexes are recorded, built, and queried. The user-visible contract (query
knobs, scoring, recall) is in
[guide/vector-search.md](../guide/vector-search.md).

The stored vector column is the source of truth; ANN indexes are derived,
per-segment aux overlays behind an engine seam. Exact flat KNN, cosine
raw-column normalization, full-precision rescoring, and creation of missing
overlays all read or regenerate from the column.

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

## Codecs

Every overlay is an IVF index: k-means centroids route a query to inverted
lists, and the lists hold one compact code per vector. The field's schema
`ann` settings pick the code (see
[guide/vector-search.md](../guide/vector-search.md#index-settings)); the
builder reads them when it builds an overlay and records the codec and its
build parameters in the overlay's `VectorAuxMeta`, so the reader and query
path never consult the schema to interpret an overlay. A codec change applies
to overlays built afterwards. Segments with different codecs for one field are
supported, not merely tolerated: each segment engine is constructed from its
own overlay's metadata, both codecs share the IVF list machinery, and the
cross-segment list ranking compares centroid distances that are in the same
metric space for both (see rotation below). RaBitQ query quantization
(`query_bits`) is not an index property; it is read from the current schema at
search time.

**IVF+RaBitQ** (default, `ENGINE_IVFRABITQ`) uses FAISS `IndexIVFRaBitQ`.
RaBitQ (Gao and Long, SIGMOD 2024) stores, per vector, the sign bits of its
residual from the list centroid plus two float factors, and estimates
distances from the code with a proven error bound; extended RaBitQ adds
magnitude bits per dimension. There is no codebook to train: building trains
only the IVF centroids, unlike PQ, which retrains per-subspace k-means
codebooks on every segment build and merge.

The error bound and unbiasedness assume the codes are taken in a randomly
rotated space. FAISS does not rotate internally: `RaBitQuantizer.h` states "It
is assumed that the Random Matrix Rotation is performed externally", and the
FAISS RaBitQ benchmark and tests wrap the index in
`IndexPreTransform(RandomRotationMatrix, ...)` themselves. Luxir therefore
builds a `RandomRotationMatrix` (orthonormal, d x d, fixed seed), rotates the
training sample and every added vector, and serializes the header as the
standard `IndexPreTransform(rotation, IndexIVFRaBitQ)`. The reader takes the
wrapper apart: it keeps the IVF index (whose lists it serves from the mmapped
file as usual) and interns the rotation in a process-wide table keyed by
content. Because every overlay of a given dims uses the same seed, all of them
share one matrix in RAM (d^2 floats, 2.25 MiB at 768 dims), and a query is
rotated once per distinct matrix rather than once per segment. The rotation is
orthonormal, so L2, inner product, and cosine (unit vectors stay unit) are
preserved, and centroid distances stay comparable across rotated and
unrotated segments in the global list ranking. On one real embedding corpus
the rotation made no measurable recall difference (the embeddings are close to
isotropic already); it is kept because the guarantee, not that corpus, is what
the codec is chosen for.

RaBitQ's estimated L2 distance is unbiased and can be slightly negative for
near vectors, so the approximate L2 score conversion clamps distances at zero
(1 / (1 + d) would otherwise turn a negative estimate into a worse score).

FAISS also ships `IndexIVFRaBitQFastScan`, which block-packs codes 32 vectors
at a time for SIMD lookups and measured about 1.5 to 2 times faster list scans
in a standalone FAISS test. It is not used yet: its lists are
`BlockInvertedLists` (padded 32-vector blocks with per-block factor regions),
which needs a second mmapped list layout; its `search_preassigned` rejects
the per-call stats sink the parallel scan uses; and the vendored FAISS 1.14.3
install omits `faiss/utils/popcount.h`, which its header includes, so it does
not compile against the installed headers without patching the port. The
codec is recorded per overlay, so FastScan can arrive as another engine id.

**IVF+PQ** (`ENGINE_IVFPQ`) uses FAISS `IndexIVFPQ` with `pq.m`
sub-quantizers of `pq.bits` bits each, unrotated, exactly as before.

## Build Policy

Build policy is per segment and size based. An IVF overlay is attempted only
when the segment has enough vectors to satisfy the codec's training floor and
enough `N * dims` scan cost to justify an ANN index. Segments below either
threshold stay on the flat-over-column path. This mixed composition is normal:
a query can use FAISS for large segments and exact column scan for small
segments.

The floors differ per codec for different reasons. PQ must train 2^bits
centroids per sub-quantizer, and FAISS wants 39 points per centroid, so 8-bit
PQ needs about 10k vectors. RaBitQ trains only the IVF centroids, which 39
points each cover from about 1.5k vectors, but training is not what binds: at
the default effort (`nlist = sqrt(N)`, `sqrt(nlist)` lists probed, about 194
candidates rescored for k=10) a single-threaded standalone FAISS measurement
found the exact scan faster than an IVF+RaBitQ probe plus rescore below
roughly 8k vectors, at both 768 dims (4k vectors: 0.25 ms exact vs 0.61 ms
IVF; 16k: 1.74 ms vs 0.38 ms) and 128 dims (4k: 0.04 ms vs 0.07 ms; 8k:
0.10 ms vs 0.06 ms). The RaBitQ floor is therefore 8192 vectors, close to the
PQ floor, and the exact path keeps small segments exact.

The training set is a strided sample across the segment's vector values, not
a prefix sample. This avoids training only on the oldest values when data
inside a segment is time ordered. The builder normalizes vectors for FAISS
when the field uses cosine and the stored column is raw.

## Memory-Mapped Index Layout

Index data stays memory-mapped at query time. The aux file holds a small FAISS
header (index parameters, IVF centroids, and the PQ codebooks or RaBitQ
rotation) followed by the inverted-list payloads (codes and vector ids) in
Luxir's own layout; the
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
the full-precision column, sorted by exact score, and collapsed to one hit per
document.

## Parallel Execution

A single query's scan and rescore run in parallel when the request executes in
parallel mode. The selected IVF lists are split into contiguous per-segment
chunks sized by their actual vector counts, and each chunk (plus each
column-scanned segment) is scanned as an independent task; the terminal
full-precision rescore runs one task per segment whose candidate bucket
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
