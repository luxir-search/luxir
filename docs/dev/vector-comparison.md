# Comparing vector search across source refs

The [Compare vector search refs workflow](../../.github/workflows/vector-compare.yml)
compares a base revision, a candidate with extra vector field settings, and the
same candidate with its defaults. It builds the release server from each ref
and measures requests over HTTP. Results go to the Actions step summary and a
14-day artifact; the workflow never posts a pull request comment.

This is a whole-server comparison. For engine-only measurements, use
`luxir_test --bench` and the vector benchmarks in
[`test/bench/VectorKnnBM.cpp`](../../test/bench/VectorKnnBM.cpp). In particular,
`BM_VectorAnnDataset` accepts a raw float32 corpus through
`LUXIR_VECTOR_DATASET`. Its codec sweep, in-process timing, and overlay byte
counts answer different questions from HTTP p50/p99 and total index size here.

## Publish dependencies once

The [Dependency image workflow](../../.github/workflows/deps-image.yml) builds
`.devcontainer/Dockerfile` through `dev`. It publishes
`ghcr.io/<repository-owner>/luxir-dev:<key>` and skips a tag that already exists.
The key is a SHA-256 of all four Dockerfile stages and their declared
COPY inputs. Compiler, base image, vcpkg pins, ports, triplets, build checks,
ASan dependencies, and final development setup therefore participate; engine
source and build concurrency do not. Unknown COPY/ADD contracts fail so the helper must be reviewed when
the Dockerfile layout changes. Ignore rules are validated by Docker at build
time, rather than included in dependency identity.

The image includes normal and ASan dependencies and the development environment.
Use it directly with the wrapper:

```bash
key=$(python3 tools/benchmarks/deps-key.py .)
export LUXIR_DEV_IMAGE="ghcr.io/<repository-owner>/luxir-dev:$key"
docker pull "$LUXIR_DEV_IMAGE"
./tools/dev-container cmake --preset container-release
./tools/dev-container cmake --build --preset container-release --target luxir
```

The same image supports `container-asan`; see [container builds](container-build.md)
for ASan and IDE setup.
Publishing runs after relevant changes reach `main`, or on a manually selected
trusted branch. It has package-write permission and must only run reviewed
Dockerfiles and dependency inputs. It does not accept an arbitrary repository
or ref as an input. Make the package public in repository/package settings so
the comparison job can pull it without registry credentials.

For each ref to compare, compute its key with the trusted helper. If a candidate
has a different dependency key, review its Dockerfile and copied inputs, put
those dependency changes on a trusted publishing branch that contains this
workflow, then dispatch Dependency image on that branch. Matching keys reuse
one image. The comparison job fails with the missing image name and publishing
workflow name instead of rebuilding vcpkg. A tag records dependency inputs, not
a guarantee of bit-for-bit builds: apt repositories can change. Refresh a
published environment by reviewing and changing its pinned inputs.

## Dispatch the comparison

In Actions, select Compare vector search refs and a trusted workflow branch.
Enter the base ref in this repository, an optional public candidate repository,
and a candidate ref. Choose a pinned dataset (DBpedia is the default). Full commit SHAs make reruns reproducible. A fork's pull
request can also be selected with `refs/pull/NUMBER/head` in this repository;
that moving ref is resolved to a SHA and recorded in the artifact.

The candidate field settings input is a JSON object merged into its vector
field. The base and candidate-default control receive no extra settings.
To compare product quantization against the default RaBitQ codec, use:

```json
{"ann":{"codec":"pq"}}
```

An empty object compares defaults only. Overrides of `type`, `dims`, and
`metric` are rejected, so all variants share the same vectors and distance
metric. Review a fork before dispatching: its build scripts and server execute
on an ephemeral hosted runner. There are no repository secrets, registry login,
write permissions, automatic pull request triggers, or persistent Actions
caches in this job. Checkout credentials are not persisted. The dependency
publisher is separate from running candidate engine code.

For a local run with Docker, Python 3.12, and a C compiler:

```bash
python3 -m venv .bench-venv
.bench-venv/bin/pip install -r tools/benchmarks/requirements.txt
.bench-venv/bin/python -m unittest discover -s tools/benchmarks -p 'test_*.py'
export BASE_REPO=owner/luxir BASE_REF=<base-sha>
export CANDIDATE_REPO=owner/luxir CANDIDATE_REF=<candidate-sha>
export DEPS_REGISTRY=ghcr.io/owner/luxir-dev
export WORK_DIR="$PWD/vector-work"  # Must not exist yet; use an absolute path.
export BENCH_PYTHON="$PWD/.bench-venv/bin/python"
export DATASET=cohere-wikipedia-en-v3-1024-100k
export CANDIDATE_FIELD_SETTINGS='{"ann":{"codec":"pq"}}'
tools/benchmarks/compare-refs
```

The shell driver fetches public sources anonymously, pulls unique dependency
images, and uses `tools/dev-container` with `container-release`. Source and
build outputs live under `WORK_DIR`; `LUXIR_CACHE_DIR` controls the usual shared
compiler cache. The harness requires Linux `/proc`, a C compiler, and a glibc
server binary runnable on the host. It uses a small bind wrapper to keep its
owned HTTP and gRPC listeners on loopback and stops each process on completion
or failure. It does not alter host firewall rules.

## Read the results

The dataset choice is shared by the workflow's `dataset` input, the local
`DATASET` environment variable, and `vector-compare.py --dataset`. The default
is `dbpedia-openai3-1536-100k`. Both presets index the first 99,000 rows and use
the last 1,000 as held-out queries, in shard/row order; `k` is 10.

| Preset | Source and pinned revision | Subset / vector column | Dimensions | License status at pinned revision |
|---|---|---|---:|---|
| `dbpedia-openai3-1536-100k` | [Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-100K](https://huggingface.co/datasets/Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-100K/tree/e8931e5eeba5b31bb9481f98687cf0ced14c2442), `e8931e5eeba5b31bb9481f98687cf0ced14c2442` | default / `text-embedding-3-large-1536-embedding` | 1536 | Card: undeclared; API: undeclared |
| `cohere-wikipedia-en-v3-1024-100k` | [CohereLabs/wikipedia-2023-11-embed-multilingual-v3](https://huggingface.co/datasets/CohereLabs/wikipedia-2023-11-embed-multilingual-v3/tree/ade45fb52bd549f5e8c065636fe4160a43c2af36), `ade45fb52bd549f5e8c065636fe4160a43c2af36` | en / `emb` (Cohere Embed v3) | 1024 | Card: undeclared; API: undeclared |

The DBpedia preset reads its original three shards. The Cohere preset reads
only `en/0000.parquet`, which contains exactly 100,000 rows. Every shard has a
pinned SHA-256 in the small `PRESETS` table in
[`vector-compare.py`](../../tools/benchmarks/vector-compare.py); downloads and
cached files are checked against it. The harness validates the actual vector
column, expected dimensions, finite values and nonzero norms before ingestion.
Cohere's measured norms range from 0.9999078 to 1.0000813, within the existing
unit tolerance. Each result records the selected preset and measured norms.
Neither pinned card nor API declares license terms. No Apache or Creative
Commons terms are inferred from the embedding model or Wikipedia source.
The harness downloads locally and does not bundle or upload either corpus;
redistribution terms need separate confirmation.

To add a preset, inspect a pinned dataset card and API, then read the required
shards to verify the column, dimensions and norms. Add one entry to `PRESETS`
with its repository, exact revision, subset, ordered shard paths and SHA-256s,
vector column, dimensions, index/query counts and exact card/API license status.
List only the shards needed for the first 100,000 rows. Add its name to the
workflow choice list and tests, and run a full comparison. The fixed 99k/1k
split is shared by these presets. Arbitrary dataset URLs are not accepted.

Unit-normalized vectors use inner product within a 0.001 norm tolerance;
otherwise the harness uses cosine. Each variant builds one ANN overlay segment
and obtains exact top-ten results from the server. Exact result ID sets must
agree across all three variants before a successful summary is written.

For adaptive probing, `nprobe=16`, and `nprobe=64`, the harness runs one warm-up
and three measured query passes. Variant order rotates every pass. Each table
value is the median of per-pass p50, p99, or mean recall, not a percentile of a
pooled latency distribution. All three servers stay alive during these passes.
Requests are serial, with two server workers by default; this is not concurrent
throughput or a codec kernel timing.

Ingest is serial by default: every server receives
`--indexing.max-inflight-batches=1`. A single HTTP stream otherwise pipelines
internal batches, which can change physical document order and the strided ANN
training sample. Serial ingest makes identical arms build identical indexes in
the same environment. Use `vector-compare.py --max-inflight-batches=0` to omit
the server flag and use its default, or choose a positive batch limit. The
selected limit and each measured pass's ordered query-result ID hash are
recorded in `results.json`. This does not promise identical bytes across CPUs
or numerical libraries.

Ingest time, merge plus ANN build time, and index size are single
measurements, prepared in base, candidate-feature, candidate-default order, and
have their own table. Index size includes all collection files, not just
compressed vectors. Latency changes are percentages relative to the base at the
same nprobe; recall changes are percentage points. Always read latency and
recall together.

`results.json` records source and binary hashes, schemas, dataset provenance,
CPU/SIMD details, individual pass latencies, and the schedule. Build timing,
server logs, overlay statistics, file sizes, and shutdown checks are also in
the artifact. Collection data directories are excluded. Keep the artifact when
a run matters; it expires after 14 days.

Hosted runners vary in load, CPU, and frequency. Interleaving reduces drift but
does not establish a statistically significant speedup. In three hosted runs
of the same two refs, absolute base p50 moved by up to 37% between runs, the
feature arm's single-shot ingest change ranged from -4% to +40%, and base
recall@10 at nprobe 16 ranged from 0.897 to 0.906. Compare only within a run,
and treat latency differences under about 10% as noise. Those earlier runs
used parallel ingest; serial ingest removes the observed document-order source
of recall variation. Other environments and numerical implementations still
need their own repeatability checks. The full development
image, two source builds, and dataset need substantial disk space;
hosted pull/build duration has to be measured on the actual runner. A selected
dataset, single segment, fixed settings, and serial requests do not represent
all search workloads. Native ARM, cold-cache performance, and ASan results are
outside this workflow.

## Filtered kNN comparison

Pass `--filtered` to `vector-compare.py`, or set the workflow's `filtered`
input. The unfiltered path and its output are unchanged when this is off.
Filtered mode compares four sequential arms: base RaBitQ, candidate RaBitQ,
base PQ (`ann.codec=pq`), candidate PQ. Omit
`candidate_field_settings`, since this mode fixes the codecs for both refs.
The workflow uses the same hosted runner and dependency images as unfiltered
comparisons. Use either 100k preset in CI; the 1M preset is for local runs.

Each indexed document gets a synthetic uniform integer rank and one integer
similarity rank per query. All ranks are unique permutations of `0..N-1`.
Uniform ranks use NumPy's seed 196 permutation, independent of the vectors.
Similarity ranks use descending float64 inner product (or cosine similarity
for a cosine dataset), breaking ties by input document ID. Thus:

- **uniform:** the first `m` random ranks, the same uncorrelated subset for
  every query;
- **aligned:** that query's `m` nearest documents by similarity rank;
- **anti-aligned:** that query's `m` farthest documents by similarity rank.

Here `m = max(10, round(N * selectivity))`, for selectivities 0.01%, 0.1%,
0.3%, 1%, 3%, 10%. Results include actual cardinality and selectivity; on the
99k preset the smallest set is 10 documents rather than 9.9. Alignment is
query-specific, not a cluster proxy. This is a deliberately strong correlation
stress test, not a model of production tenant filters. All synthetic fields
are explicit `int` columns with `index: range`. Queries use Luxir's ordinary
pre-filter syntax, for example `"filter":["filter_q0_i:[0 TO 99]"]`, beside
`"query":{"knn":...}`. Anti-aligned filters select the upper rank interval.
Every query is checked against server `exact:true` top-10 using the identical
filter, computed once per query/kind/selectivity on the first arm and shared
by all arms. Every arm also measures exact controls and requires recall 1.0.
Filter cardinalities are checked against server counts. Underfilled ANN
responses contribute missing neighbors to the recall denominator of ten; duplicate
IDs and overfilled responses are errors.

The default sample is the first 32 held-out queries; `--filtered-queries` can
change it. A small sample bounds the number of synthetic indexed columns and
exact requests. Percentiles from 32 queries are coarse, especially p99; raw
per-pass latencies and ordered-result hashes are retained. Filter setup and
similarity ranking are outside latency measurement. Serial ingest remains 1.
Each cell has one warm-up and three timed passes, reporting medians of pass
recall@10, p50 and p99. Arms run sequentially, so load drift remains a limitation.
The effort sweep is default/adaptive, 1, 4, 16, 64, 256, 1024 nprobe. The summary
records the least *swept positive nprobe* reaching mean recall >= 0.95, or
`not reached`; adaptive effort is reported separately and is not ordered as 0.
This is no guarantee of a global minimum between sweep points. An explicit
nprobe requests effort; the candidate can still choose exact search internally.

`cohere-768-cos` uses 1,000,000 training vectors and a separate 1,000-query
file from [YoKONCy/Cohere-1M-wikipedia-768d](https://huggingface.co/datasets/YoKONCy/Cohere-1M-wikipedia-768d/tree/de34a7af7f436d7aceb4fecdda01490e552efdde),
revision `de34a7af7f436d7aceb4fecdda01490e552efdde`. The preset pins both file
SHA-256s, dimensions (768), sizes, row counts, cosine metric, and the mirror's
Apache-2.0 license declaration. The first 32 test queries are used by default;
no train vectors are held out. This preset requires filtered mode. Existing
100k presets retain their original split and metric detection.

For a correctness smoke, use a 100k preset and `--filtered-queries 8`, pointing
both binary options at the same server. Exact controls must have recall 1.0;
compare each codec's base/candidate ordered-ID hashes across all measured
passes. Smoke latency under concurrent work is not meaningful.

```bash
gh workflow run vector-compare.yml -f filtered=true \
  -f dataset=dbpedia-openai3-1536-100k \
  -f base_ref=main -f candidate_ref=refs/pull/NUMBER/head
```

For a local 1M comparison, build both binaries first, then run:

```bash
python3 tools/benchmarks/vector-compare.py --filtered --dataset cohere-768-cos \
  --base-binary /path/to/base/luxir --candidate-binary /path/to/candidate/luxir \
  --cache /path/to/datasets --output /path/to/results
```

The 1M dataset and four indexes need substantial disk space and time. Collection
information and shutdown checks are retained, but collection data directories
are removed after each arm, including failure, once its server has stopped.
Keep the results artifact; do not upload collection data or downloaded corpora.
