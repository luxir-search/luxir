# Vector Search Internals

Design of the vector indexing and search path: how per-segment FAISS aux
indexes are recorded, built, and queried. The user-visible contract (query
knobs, scoring, recall) is in
[guide/vector-search.md](../guide/vector-search.md).

The stored vector column is the source of truth; ANN indexes are derived,
per-segment aux overlays behind an engine seam. Exact flat KNN, cosine
raw-column normalization, full-precision rescoring, and creation of missing
overlays all read or regenerate from the column.

`truncate_dims` (Matryoshka prefix truncation) is applied before the column:
the index handler validates the input length (`dims`, or `truncate_dims` for
an already truncated vector), cuts the vector to its prefix, then runs cosine
normalization, and the query parser cuts the query vector the same way.
`VectorFieldType::storedDims()` is the stored dimension and `inputDims()` the
declared input one, so the column, overlays, exact scan and rescoring see
only the truncated vector and need no special casing; schema `ann` settings
such as `pq.m` are validated against `storedDims()`. Truncation requires
`dims`, since stored lengths can no longer tell models apart, and cosine,
under which a prefix cut here and one renormalized by the provider score the
same.

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
`ann` settings pick the code (RaBitQ by default, or explicit PQ; see
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
distances with probabilistic error bounds under its random-rotation model;
extended RaBitQ adds
magnitude bits per dimension. There is no codebook to train: building trains
only the IVF centroids, unlike PQ, which retrains per-subspace k-means
codebooks on every segment build and merge.

The error bound and unbiasedness assume the codes are taken in a randomly
rotated space. FAISS does not rotate internally: `RaBitQuantizer.h` states "It
is assumed that the Random Matrix Rotation is performed externally". Luxir
rotates with a seeded FHT-Kac transform (the rotator of the RaBitQ authors'
library) in `util/VectorRotation.h`, shared by the builder and the
reader/query code. It uses exactly d coordinates, with no padding. Four
rounds each apply random signs, then a normalized Walsh-Hadamard transform
on the largest power-of-two block fitting in d, alternating head and tail.
For non-power-of-two d, each round then pairs the first floor(d/2)
coordinates with the last floor(d/2) coordinates in orthogonal
sum/difference Kac steps. For odd d, the middle coordinate is covered by both
Hadamard blocks. Power-of-two d skips Kac.

The signs use SplitMix64 from the recorded seed, consuming each word low bit
first continuously across rounds; a set bit negates the coordinate. The
rotation kind fixes this algorithm and bit ordering. Every step is orthogonal,
so norms, inner products, cosine and centroid distances remain comparable
with unrotated PQ segments. Application costs O(d log d), with O(d) cached
signs, replacing the dense d x d matrix and its quadratic work. FAISS's
HadamardRotation pads to the next power of two, adding 33% code bytes at
768, 1536 and 3072 dimensions; avoiding that padding preserves code density.

Training samples and added vectors use the same transform. Headers are bare
`IndexIVFRaBitQ`; metadata stores rotation kind and seed, with no matrix.
Readers intern by (kind, dims, seed), and queries rotate once per distinct
transform. A fixed seed shares the transform across overlays of one dimension.
Confidence intervals remain empirical, rather than a deterministic guarantee.
On five 1M corpora (128 to 1536 dims, L2, IP and cosine) it matched the dense
rotation's recall and latency at 1 and 2 bits, within the run-to-run
variation of the k-means partition, and cut 1-bit build CPU by up to 23% (NQ
1536: vector build 12.0 to 8.2 s).

Each IVF engine slot binds an immutable prepared query: the canonical query
for PQ, or the cached rotated query for RaBitQ, plus the current schema's
query quantization bits. Coarse assignment and both document and group scans
use that same index-space query. Each scan call owns its selector and distance
computer, with query quantization taken from the prepared query, so filtering,
deeper rounds, and parallel list chunks share the same preparation policy
without sharing mutable distance-computer state. Document and group scans share one traversal
and result handler. PQ retains FAISS's optimized code kernel; RaBitQ evaluates
its 1-bit estimate and gates extra-bit work in Luxir. Exact column scans and
terminal rescoring retain the canonical query.

RaBitQ's estimated squared L2 distance can be negative. Internal ordering
scores use `1 / (1 + d)` for `d >= 0` and `1 - d` for `d < 0`, preserving
ordering across segments and bounded cuts instead of tying negative estimates
at 1. The common bounded handler inverts this map outward when setting its
competitive distance threshold, admitting equal scores for identity ties.
These approximate scores may exceed the public L2 range
`[0, 1]`; terminal rescoring from column vectors restores exact scores before
the final cut, similarity filtering, and returning results. Exact column
witnesses bypass the bounded approximate heap, so an inflated estimate cannot
evict an exact candidate. Public L2 scoring remains `1 / (1 + squared_distance)`.

FAISS also ships `IndexIVFRaBitQFastScan`, which block-packs codes 32 vectors
at a time for SIMD lookups and measured about 1.5 to 2 times faster list scans
in a standalone FAISS test. It is not used yet: its lists are
`BlockInvertedLists` (padded 32-vector blocks with per-block factor regions),
which needs a second mmapped list layout, and its `search_preassigned` rejects
the per-call stats sink the parallel scan uses. The codec is recorded per
overlay, so FastScan can arrive as another engine id.

**IVF+PQ** (explicit, `ENGINE_IVFPQ`) uses FAISS `IndexIVFPQ` with `pq.m`
sub-quantizers of `pq.bits` bits each, unrotated. PQ's automatic `m` is the
largest divisor of dims under `max(32, dims / 8)` that leaves at least four
dimensions per sub-quantizer, for every metric. If none does, use the largest
divisor under the same cap. Explicit `m` is unchanged. Automatic `m` depends
only on stored dims, including inferred dims, so it is stable across merges
of the same dimension. It selects 48 / 96 / 120 / 192 / 384 at
384 / 768 / 960 / 1536 / 3072 dims.

The reader skips FAISS precomputation during deserialization. For L2 with
`by_residual`, it explicitly enables the `nlist * m * 2^bits`-float table
only when its bytes are at most `ntotal * dims * 4`, the raw vector bytes.
Otherwise it disables the table and FAISS computes tables per list. This
bypasses FAISS's global 2 GB cap. With default sqrt(N) lists and m near d/8,
the table is about 32/sqrt(N) of raw bytes; the bound primarily constrains
explicit large nlist. IP/cosine has no resident L2 table.
`IndexReaderAuxTest.cosineSkipsL2PrecomputedTable` pins both metric behavior
and the size boundary.

The basis is the Cohere-768 cosine [measurement below](#codec-measurements):
`m=96` improves recall over `m=32`, with code bytes and per-code scan work
growing linearly with `m` (32 -> 96 bytes at 8 bits). The leading k-means
work per training vector per iteration, and encode work per vector, remain
`256 * dims` at 8 bits: splitting the dimensions into more sub-quantizers
does not multiply that arithmetic. This does not establish unchanged
wall-clock build time; the build measurement below used `m=32`.

### Default codec

RaBitQ is the default, chosen on one-segment indexes at default probes plus
16 to 256 lists, single client: SIFT-128 L2, MIRACL-768 cosine, GIST-960 L2,
Cohere-1024 IP, NQ-1536 cosine, Jina v5-small MS MARCO embeddings truncated
to 256 / 384 / 512 / 1024 dims (cosine), all at 1M vectors, and OpenAI
text-embedding-3-large 3072 IP at 500k.

From 384 dims up, 1-bit RaBitQ ran at about 0.5-0.8x PQ latency at equal or
higher recall and built with equal or less CPU (0.39x at 3072 dims). Its
intervals size the candidate pool per query, while PQ's bounded count,
fitted on one 1024-d IP corpus, left recall behind elsewhere: at 256 lists
PQ reached .872 recall@10 on MIRACL (.975 with 1000 candidates), .710 on
GIST at m=32 (.954), .949 on Jina-384 and .960 on OpenAI-3072, against
.981-.997 for 1-bit RaBitQ. At 128 dims 1-bit RaBitQ took 1.16x / 1.30x PQ
latency at k=10 / k=100, and 2-bit 1.03x / 1.02x at 0.55x PQ's build CPU.

Automatic bits (`0`) resolve to 2 below 384 stored dims and 1 at or above.
On the same model truncated to 256 / 384 / 512 / 1024 dims, 2 bits cut k=100
latency by 23% / 13% / 9% / 1% (21% on SIFT-128) but k=10 latency only
below 384, while costing 1.5-1.8x the build CPU. 4 bits never beat 2.

GIST L2 with PQ m=32 topped out at .71 recall@10 at 256 of 1000 lists;
m=120, now the automatic choice, reaches .94-.98.

### Codec measurements

These measurements predate the dimensions-scaled default probe policy and
the dimensions-scaled automatic PQ `m` for IP/cosine.
Here, "default" means `sqrt(nlist)` lists at every dimensionality, without
the multiplier now applied above 128 dimensions. The default-recall and
latency numbers below describe that earlier policy.

`Tuning/BM_VectorAnnDataset` (test/bench/VectorKnnBM.cpp) indexes a real
corpus through the normal request path into 4 segments, builds every overlay
with the production policy, and measures recall@10 of 200 held-out queries
against the exact path plus serial single-query latency (gcc-release,
i7-14700KF, shared with other builds). Two corpora:

- 485,659 Cohere `multilingual-22-12` embeddings of Simple English Wikipedia
  (768 dims, cosine).
- SIFT1M (1,000,000 x 128, L2).

In this historical 768-dimension campaign, the tested 1-bit pools reached the
same recall as the higher-bit codecs: 1, 2,
and 4 bits all give 0.893 / 0.911 / 0.960 / 0.986 at nprobe default / 32 / 64
/ 128, at the tested rescore pools. PQ at its then-automatic `m=32` (32 code bytes) never
passed 0.88, even with 500 rescored candidates; PQ with `m=96` (96 bytes,
close to RaBitQ's 104) reached 0.886 / 0.902 / 0.950 / 0.970. The then-current bounded scans measured 1.2 ms for RaBitQ versus 0.66 ms for
PQ `m=96` at default nprobe. Those measurements predate shared floors,
interval selection and amplitude storage; they do not establish a universal
codec ranking or that 1-bit recall is generally list-limited. At 128 dims (SIFT) the
defaults are close in recall (0.953 vs 0.960 at the default nprobe; 0.989 vs
0.999 at nprobe 128, where 2-bit RaBitQ or 500 candidates reach 0.9985 and
0.9965) and PQ is again faster. Overlay builds, including training and
coarse assignment, took 10.2 s (RaBitQ, 140 B/vector) vs 16.7 s (PQ m=32,
55 B/vector) at 768 dims and 10.4 s vs 12.9 s at 128 dims. Bytes per vector
include the 8-byte id and per-overlay headers (centroids, and for RaBitQ the
stored d x d rotation).

The Cohere-768 cosine comparison motivating PQ's automatic `m=96` is:

| PQ m | Code bytes/vector (8 bits) | Recall@10, earlier default probes | Serial latency, earlier default probes | Recall@10, nprobe 64 |
|---|---|---|---|---|
| 32 | 32 | 0.761 | 0.46 ms | 0.784 |
| 96 | 96 | 0.886 | 0.66 ms | 0.950 |

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
points each cover from about 1.5k vectors, but training is not what binds. The
original floor calibration also predates the dimensions-scaled probe policy:
at `nlist = sqrt(N)`, `sqrt(nlist)` lists probed at every dimensionality and
about 194 candidates rescored for k=10, a single-threaded standalone FAISS
measurement found the exact scan faster than an IVF+RaBitQ probe plus rescore below
roughly 8k vectors, at both 768 dims (4k vectors: 0.25 ms exact vs 0.61 ms
IVF; 16k: 1.74 ms vs 0.38 ms) and 128 dims (4k: 0.04 ms vs 0.07 ms; 8k:
0.10 ms vs 0.06 ms). The retained RaBitQ floor is 8192 vectors, close to the
PQ floor, and the exact path keeps small segments exact.

The training set is a strided sample across the segment's vector values, not
a prefix sample. This avoids training only on the oldest values when data
inside a segment is time ordered. The builder normalizes vectors for FAISS
when the field uses cosine and the stored column is raw.

Raw-cosine training samples are normalized in place in their owned buffer.
RaBitQ rotates the sample in place in row partitions; PQ needs no transform.
Each add scratch buffer is limited by `ivfAddChunkBytes` (16 MiB by default),
capped by available vectors with a minimum of one vector. Add uses up to two
buffers for raw cosine plus rotation. Untransformed PQ add passes column bytes
directly to FAISS. The serialized header's `nprobe` is unused by Luxir:
searches supply explicit probe parameters or selected lists.

### Build parallelism

OpenBLAS is built serial (with locking for concurrent callers), and a build
gets its GEMM parallelism from TBB tasks in the indexing arena instead of a
BLAS thread pool. A threaded BLAS would reach the query path: every level-3
call holds a per-variant lock for its duration, so a query's rotation or
coarse search would queue behind a build's GEMMs, and on x86 OpenBLAS a
one-column SGEMM still asks for a thread team at 768 dims and up. Measured on
whole builds, partitioning across tasks was also faster than the thread pool
and used far less CPU.

The arena observer pins FAISS OpenMP to one thread on every arena entry,
including masters. All build and query parallelism comes from TBB tasks.
The build partitions these steps in the indexing arena, isolating the caller
so it cannot pick up an unrelated merge or flush while waiting:

- Coarse assignment during training. The IVF's actual quantizer is an
  `IndexFlat` subclass whose searches split rows, covering both k-means and
  residual-training assignment.
- Add work for every codec. Each row slice normalizes when needed, rotates
  for RaBitQ, coarse-assigns with the base `IndexFlat::search`, and calls
  `encode_vectors` with list numbers. One serial `add_sa_codes` per chunk
  preserves input order within each inverted list.
- RaBitQ training-sample rotation, in disjoint row slices in place.
- PQ codebook training, one task per sub-quantizer with its own sub-vector
  buffer, clustering instance and flat L2 index. This mirrors FAISS's default
  training path; residual precomputation follows after all tasks finish.

Row partitions have at least `ivfBuildMinRowsPerTask` rows (by default
`ceil(distance_compute_blas_threshold / dims)` to retain FAISS's BLAS path),
up to four tasks per compute slot. Add chunks hold the larger of 16 MiB of
input vectors and four times the arena compute width times that row floor,
capped by available vectors, so scratch capacity scales with core count.
The build subclasses serialize through FAISS's `dynamic_cast` dispatch as
plain `IndexFlat` and `IndexIVFPQ`; readers need no build-specific types.

## Memory-Mapped Index Layout

Index data stays memory-mapped at query time. The aux file holds a small FAISS
header (index parameters, IVF centroids, and the PQ codebooks) followed by
the inverted-list payloads (codes and vector ids) in Luxir's own layout; the
reader decodes only the header into memory and serves list scans directly from
the mmapped file. Residency of the bulk index data is therefore managed by the
OS page cache, like the vector column itself, rather than forced into process
RAM.

RaBitQ overlays also store upward-rounded float32 error amplitudes after the
codes in inverted-list order: sigma1 for 1-bit, sigmaB for multi-bit
(4 bytes/vector for every width). The multi-bit gate reads sigma1 from the
code's own f_error; the side amplitude is read only after that gate. PQ has
no amplitude section.
The builder computes these in the encoding row pipeline from the rotated
original, assigned centroid and actual code. SigmaB is the residual
reconstruction error norm divided by sqrt(max(1, D-1)), with a factor of two
for squared L2. Sigma1 removes FAISS's fixed 1.9 confidence multiplier from
its error factor. These amplitudes support empirical confidence intervals;
they are not deterministic error bounds. The reader exposes a mmap view.
The reader validates finite, non-negative amplitudes once at open and exposes
per-list spans for sequential, unchecked scan loads. Metadata and footer
version 3 validate the component count and file size;
older formats fail on open and must be rebuilt.

Before taking apart a RaBitQ wrapper, the reader checks the header's bit
count against the overlay metadata. Both codecs also check the header's list
count against that metadata, in addition to checking the list footer against
the header. Mismatches reject the overlay with the conflicting values in the
diagnostic.

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
arbitrary candidates. `nprobe` selects lists; codec gates and terminal
selection determine the work performed on their values.

Candidate selection has independent bounded, interval and exact channels.
PQ defaults to `k + 24 + ceil(15 * k^0.8)`; explicit `refine_candidates`
selects the bounded policy for every approximate codec. Default RaBitQ uses
intervals for every bit width. Exact column slots supply distinct-unit top-k
results outside the approximate quota.

The bounded channel uses a shared R-th estimate floor. Its fresh DEPTH epochs
exclude banked ranks and reset that floor. Its document budget, multiplicity
inflation and group member caps apply only to bounded candidates. Interval
selection is complete after traversing the selected lists, independent of
retained count, and never requests DEPTH. BREADTH visits only newly allocated
lists and preserves interval witnesses and retained identities. A mixed query
must satisfy the bounded channel's own obligation; interval hits cannot fill
its quota. The result contract reports bounded completion separately from
remaining breadth. Insufficient distinct units can trigger more breadth:
bounded Groups continue BREADTH when the document pool is full but holds
fewer than k groups.

For RaBitQ, the query-owned collector tracks each unit's best lower endpoint
and the k-th highest of those endpoints. A unit is a (segment, document) pair
or a canonical CollapsePlan group key, including missing-key policy. A value
is retained only while its upper endpoint reaches both the global witness
floor and its own unit's lower endpoint. Multiple values of one unit can
remain competitive. PQ point estimates never witness the true k-th score.
Exact column scores may witness it. Document units use a flat hash map from
packed (segment, document) to reusable stable slots; only Groups copy and hash
canonical CollapsePlan keys. Map, slot and key storage are request-accounted.

One-bit uses its single estimate with `sigma1 * g_error * 1.9`. Multi-bit first
tests the optimistic 1-bit endpoint with gate epsilon 1.9, then constructs the
full-bit interval with `sigmaB * g_error * 1.9`. The gate uses the witness floor
for intervals and the R-th estimate floor for bounded collection. For L2 the
raw distance endpoints are mapped in reverse order through the score transform,
with outward rounding and upper scores clamped to 1. Scans reject in distance
space against a cached outward threshold, converting only when the floor
changes. Only admitted records pay for score mapping and outward endpoints. Both constants are empirical confidence choices,
validated on IP at 128 and 1024 dimensions; they are not containment guarantees.

Terminal scoring starts with the best estimated value per unit, choosing k
distinct units. All bounded candidates are mandatory in this stage too (bounded
Groups nominations expand to their whole document). Exact scores are reused.
Then `T = max(k-th exact seed score, pessimistic witness floor)` selects the
remaining interval values by `upper >= T`. A value whose upper score is below
its unit's best known exact score is also skipped. Both stages read in segment
and value-rank order, with disjoint column reads and a shared exact-score cache.
Final unit collapse and similarity filtering use exact scores.

There is no interval retention cap. `RequestMemResource` charges records,
canonical unit keys, witness heaps, pending batches and terminal selection
storage to the request's existing opt-in byte ceiling. Exceeding it throws
`request_memory_exceeded`, including from scan tasks; cancellation and joining
precede destruction of shared state. Old bounded candidate and breadth limits
remain separate existing policies.

### Filtered Search: Exact or IVF

A filter reaches kNN as a materialized per-segment domain, so its size is
known before any scan. IVF budgets breadth against every live vector and
widens until enough eligible units appear: with a fraction s of the vectors
eligible it scans about need / s codes, each paying the selector's value-rank
resolve and domain probe. Exact search scores only the eligible vectors and
probes the domain once per column vector. `KnnQuery::chooseExactSegments`
prices both per overlay segment, in units of one dimension of an exact score:

- exact: eligible vectors x dims, plus a domain probe per column vector;
- IVF: centroid ranking (nlist x dims) plus the expected scan fraction x live
  vectors x (a cost per code byte + the selector cost).

The constants were fitted from forced IVF and forced exact runs of
`Tuning/BM_VectorKnnFiltered` (500k 128-d vectors, k=10, filters keeping 0.01%
to 10%). There the choice picked the faster arm at every PQ point: exact up to
0.1% (2.1 ms against 2.5 ms for IVF, and 1.5 ms against 5.3 ms at 0.01%) and
IVF from 0.3%. RaBitQ used IVF from 0.1%; at 0.01% it picked exact at 1.5 ms
and recall 1.0 over IVF at 0.84 ms and recall 0.87. At these sizes the exact
arm is dominated by the per-vector domain probe of the column walk.

The same benchmark reads real corpora through `LUXIR_VECTOR_DATASET`, and its
`scan` argument pins `min_scan_fraction` to find the IVF effort a filter needs
for a given recall. Under selective filters IVF's default effort loses recall
(recall@10 of 0.58 to 0.87 below 1% on the corpora below), because the k nearest
filtered vectors lie much deeper than the k nearest overall. The fair rival of
exact search is therefore IVF at the least effort reaching recall 0.95. On
Deep 5M (96-d) and Cohere, GIST and arXiv 1M (768 to 1536-d), four segments,
both codecs and filters keeping 0.01% to 3%:

- 768 to 1536-d: exact is cheaper up to about 0.3% to 1%, and the choice
  takes it there; above, it takes IVF.
- Deep: IVF is cheaper at every selectivity, since the probe walk over 5M
  vectors costs about 19 ms; the choice takes IVF except with PQ at 0.01%.

Against those recall-matched costs the choices averaged 1.05x the cheaper
strategy (geometric mean), at most 1.9x: exact for Deep PQ at 0.01% (18.9 ms
against IVF's 10.3 ms at recall 0.99), and default-effort IVF for arXiv RaBitQ
at 1% (recall 0.93; 0.95 would cost 17.9 ms against exact's 9.5 ms). Refitting
the constants to those corpora's measured nanosecond costs did not improve on
this. The same runs show default deepening (one list per round) can cost more
than a pinned full scan, for example 19.4 ms against 6.7 ms for Cohere PQ at
0.01%.

The expected scan fraction is the breadth budget, raised to need / eligible,
where need is the bounded pool (or k units for RaBitQ intervals) in vectors and
eligible counts the eligible vectors of the segments still on IVF. Moving a segment to exact
shrinks that pool for the rest, so the choice repeats until stable. Eligible
vectors are the domain's documents times the segment's vectors per document,
exact for dense single-valued fields. Unfiltered segments stay on IVF, since
a code costs less than a full-precision vector.

The estimate assumes the filter is spread across lists. A filter correlated
against the query (its documents far from it) makes IVF deepen through lists
with no match. The composite engine accounts scanned codes and centroid
rankings in the same units, and when a widen round would deepen after spending
more than exact search of the IVF segments costs, prepare restarts with those
segments exact. The restart bounds a query at about twice the cheaper strategy.

## Parallel Execution

A single query's scan and rescore run in parallel when the request executes in
parallel mode. The selected IVF lists are split into contiguous per-segment
chunks sized by their actual vector counts, and each chunk (plus each
column-scanned segment) is scanned as an independent task; the terminal
full-precision rescore runs one task per segment whose candidate bucket
exceeds a small grain (smaller buckets, including the common small-`k`
single-segment pool, fold inline on the calling thread).

All IVF chunks and segments share a bounded cut per fresh epoch and persistent
interval witness state for the query. Task-local handlers batch admissions and
canonical keys; group bounded handlers keep their local capped admission.
Serialized updates publish monotone scalar floors. PQ uses its optimized
kernel with floor refreshes between 256-code blocks. RaBitQ uses its distance
computer in the common traversal. Peak and terminal retained counts, seed and
remaining reads, gate rejections and extra-bit evaluations accompany the
selector-passing scan count in the benchmark stats seam.

Task boundaries do not depend on execution mode, and bounded cuts use a total
identity order. RaBitQ confidence pruning can nevertheless change work and rare
approximate misses with scheduling, since the floor seen during a scan depends
on which tasks have published results.

## What This Means for Users

- Bulk ANN index data is paged by the OS rather than loaded into process
  RAM, so indexes larger than RAM still work.
- Because the column is the source of truth, ANN indexes can be rebuilt or
  retuned per segment without reindexing any documents.
- Segments below the training threshold, and segments whose build failed,
  are searched exactly from the column. A failed build leaves no partial
  state.
- Exact results retain deterministic ordering across execution modes.
  Approximate confidence pruning can produce different candidates as task
  scheduling changes the shared floor.
