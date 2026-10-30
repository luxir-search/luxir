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

The Docker context must include `test/blas_lapack_test.cpp` when the Dockerfile
copies it. Use a tree with the corresponding Docker ignore fix before building.
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
For a candidate implementing the opt-in PQ4 codec, use:

```json
{"ann":{"codec":"pq4"}}
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
export CANDIDATE_FIELD_SETTINGS='{"ann":{"codec":"pq4"}}'
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

Ingest time, ANN build time, and index size are single measurements, prepared
in base, candidate-feature, candidate-default order. Index size includes all
collection files, not just compressed vectors. Percent changes are relative
to the base at the same nprobe; recall changes are relative percentages, not
percentage-point differences. Always read latency and recall together.

`results.json` records source and binary hashes, schemas, dataset provenance,
CPU/SIMD details, individual pass latencies, and the schedule. Build timing,
server logs, overlay statistics, file sizes, and shutdown checks are also in
the artifact. Collection data directories are excluded. Keep the artifact when
a run matters; it expires after 14 days.

Hosted runners vary in load, CPU, and frequency. Interleaving reduces drift but
does not establish a statistically significant speedup. The full development
image, two source builds, and dataset need substantial disk space;
hosted pull/build duration has to be measured on the actual runner. A selected
dataset, single segment, fixed settings, and serial requests do not represent
all search workloads. Native ARM, cold-cache performance, and ASan results are
outside this workflow.
