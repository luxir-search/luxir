# CPU BLAS backends and vector build threads

Luxir uses FAISS for IVF training and vector assignment. RaBitQ also applies a
dense rotation to training vectors and batches of added vectors through FAISS's
BLAS calls. These operations can use CPU math libraries tuned for AMD x86-64 or
ARM64. Exact column scoring uses Luxir's own SIMD kernels and is not changed by
selecting another BLAS provider.

This page covers Linux. On macOS, FAISS uses Apple's Accelerate and OpenBLAS
is not built, so `LUXIR_BLAS_VENDOR` and `LUXIR_BLAS_ROOT` do not apply there
(see [build setup](build-setup.md)).

## Default OpenBLAS build

The dependency manifest enables OpenBLAS's `threads` and `dynamic-arch`
features. The overlay uses pthread workers, C LAPACK, and 32-bit BLAS/LAPACK
integers (LP64). Its build ceiling is 256 workers; this is a capacity limit,
not the number of workers Luxir requests. CPU kernels are selected at runtime
on both x86-64 and ARM64. The ARM64 build retains an ARMv8-A baseline.

The overlay also builds OpenBLAS with `THREAD_TIMEOUT=12`: an idle pthread
worker spins for 2^12 cycles before sleeping instead of upstream's 2^28 (tens
of milliseconds). A FAISS build alternates GEMMs with OpenMP loops on the same
cores, and workers still spinning after a GEMM starve the OpenMP team that runs
next; with upstream's spin, a 300k x 768 IVF+PQ overlay build on 4 cores took
6.0 s instead of 3.4 s. `OPENBLAS_THREAD_TIMEOUT` still overrides it at
runtime.

Rebuild dependencies after updating the manifest, then reconfigure and rebuild
Luxir using the checkout's [native](build-setup.md),
[container](container-build.md), or [ARM64](arm64-build.md) workflow. This
includes checkouts configured against preinstalled dependencies
(`VCPKG_MANIFEST_MODE=OFF`): their OpenBLAS must be rebuilt from the overlay.
An old single-threaded OpenBLAS installation is rejected at startup. Setting a
thread environment variable cannot add threading support to that library.

## Thread ownership

Luxir establishes BLAS settings once per process before engine work. Embedders
that use engine components without `LuxirNode` must call
`initializeNumericalLibraries()` before starting their workers. It does
not change the process-wide OpenBLAS worker count around individual index
builds, which would race with other builds and searches.

| Setting | Meaning |
| --- | --- |
| `OPENBLAS_NUM_THREADS` | Highest-priority OpenBLAS worker budget. |
| `OPENBLAS_DEFAULT_NUM_THREADS` | OpenBLAS worker budget when the setting above is absent. |
| `GOTO_NUM_THREADS` | Legacy OpenBLAS fallback when both settings above are absent. |
| `OMP_NUM_THREADS` | OpenBLAS fallback when all three settings above are absent; also supplies the OpenMP runtime's default. |
| `LUXIR_VECTOR_BUILD_THREADS` | FAISS OpenMP team budget for each vector overlay build; defaults to the available CPUs. |
| `BLIS_NUM_THREADS` | Required explicit BLAS budget when using AOCL. |

With none of the OpenBLAS thread variables set, Luxir requests one BLAS worker
per available CPU (capped at the library's build limit). "Available CPUs" is
TBB's count, which honors both the affinity mask and a cgroup CPU quota; the
OpenMP runtime's own default honors only the affinity mask, so under a
`--cpus` container limit it would size teams by the host.
Only the highest-priority present OpenBLAS setting is parsed. Thread counts
must be positive integers, including `OMP_NUM_THREADS` when selected as the
OpenBLAS fallback; nested OpenMP lists are not accepted in that case.
OpenBLAS requests above its compiled
capacity fail rather than silently running with a smaller worker count.

The vector-build scope sets the calling task's OpenMP budget and restores it
on return, including exceptional exits. It does not change the BLAS worker
pool. With ArmPL, the same OpenMP budget also controls ArmPL calls made in that
scope; `OMP_NUM_THREADS` is required at launch for calls outside the scope.

Commit-time overlay construction walks segments serially on the committing
task and blocks the refresh, so the defaults give each build the whole CPU
budget. A build alternates between the two pools: FAISS issues its GEMMs
(coarse k-means, list assignment, the RaBitQ rotation) from the calling thread,
and its OpenMP team runs the encoding and reduction loops in between. Both are
needed: IVF+RaBitQ is almost entirely GEMM, while IVF+PQ at small sub-vector
widths spends most of its time in OpenMP encoding. Narrow either budget when
several builds (concurrent merges) or heavy query load share the cores:

```bash
OPENBLAS_NUM_THREADS=2 LUXIR_VECTOR_BUILD_THREADS=2 \
  ./build/gcc-release/bin/luxir_test --bench \
  --benchmark_filter='BM_VectorIvfBuild'
```

When comparing settings, vary one layer at a time and keep the
number of simultaneous builds in the experiment fixed. TBB arena limits do
not constrain BLAS or OpenMP workers; concurrent builds, OpenMP teams, and BLAS
teams can compete for the same cores and memory bandwidth. Do not change the
process environment or vendor thread settings after engine workers start.

## Selecting a provider

`LUXIR_BLAS_VENDOR` selects `OpenBLAS` (default), `AOCL`, or `ArmPL` at CMake
configure time. `LUXIR_BLAS_ROOT` selects an installation containing `lib/` or
`lib64/` (AOCL also searches `lib_LP64/` and `lib/LP64/`). It is optional for
OpenBLAS and required for either vendor backend. Vendor headers must come from
the same LP64 installation as the libraries; configuration checks their integer
width. Both vendor configurations require Linux, GCC, shared vendor libraries
(`.so`), and `-DLUXIR_STATIC_RUNTIME=OFF` to use the shared compiler/OpenMP runtime.
Supply these options to the appropriate target architecture's normal CMake
workflow. Use a separate build directory for each provider.

| Provider | Target | Required libraries |
| --- | --- | --- |
| OpenBLAS | x86-64 or ARM64 | Pthread `openblas`, including C LAPACK, LP64. |
| AOCL | x86-64 | Shared LP64 `libblis-mt.so` and `libflame.so`. |
| ArmPL | ARM64 | Shared `libarmpl_lp64_mp.so`, built for the GCC/libgomp runtime used by Luxir. |

For example, on an AMD host with AOCL installed:

```bash
cmake --preset gcc-release -B build/gcc-release-aocl \
  -DLUXIR_BLAS_VENDOR=AOCL -DLUXIR_BLAS_ROOT=/opt/aocl \
  -DLUXIR_STATIC_RUNTIME=OFF
cmake --build build/gcc-release-aocl
BLIS_NUM_THREADS=4 LUXIR_VECTOR_BUILD_THREADS=1 \
  ./build/gcc-release-aocl/bin/luxir_test --bench \
  --benchmark_filter='BM_VectorIvfBuild'
```

For ArmPL, use `-DLUXIR_BLAS_VENDOR=ArmPL` and the target ARM64 installation
root with the ARM64 build workflow, overriding `LUXIR_STATIC_RUNTIME=OFF` for
the vendor configuration. Launch with explicit `OMP_NUM_THREADS` and
`LUXIR_VECTOR_BUILD_THREADS` budgets. Do not use an ILP64 library or mix a
vendor's incompatible OpenMP runtime with GCC's libgomp. A cross build needs
target-architecture libraries in its provider root, not host libraries.

The pinned FAISS package embeds OpenBLAS in its exported link dependencies for
both BLAS and LAPACK. Luxir replaces both references when selecting a provider
and retains FAISS's other dependencies. Setting CMake's `BLA_VENDOR` alone
does not replace these already-exported dependencies. The default dependency
workflow still builds OpenBLAS; optional provider installations are supplied
separately. See [AMD AOCL](https://www.amd.com/en/developer/aocl.html) and
[Arm Performance Libraries](https://learn.arm.com/install-guides/armpl/).

## Validation and comparison

Run `build/<preset>/bin/blas_lapack_test` and the vector codec/index-builder
tests against each selected provider. For OpenBLAS, run the standalone check in
separate processes with `OPENBLAS_NUM_THREADS=1` and `=2`, passing the same
number as its argument to assert the effective worker count. Validate RaBitQ and PQ, the supported metrics,
merge rebuilds, and concurrent callers. Run the applicable ASan checks before
committing engine changes. An ARM64/QEMU smoke run validates target execution
and numerical behavior; measure performance on native ARM64 hardware.

Use the Release `BM_VectorIvfBuild` and `BM_VectorIvfIncrementalBuild`
benchmarks for construction, and the vector recall benchmarks for downstream
quality. Compare the same corpus, codec, sample size, dimensions, and thread
budget. Record build latency, throughput, peak memory, and concurrent-query
latency. Wider teams and vendor libraries are experiment choices, not a
guaranteed improvement. Floating-point accumulation can differ between math
libraries, so validate recall and numerical tolerances rather than assuming
identical centroid bytes across providers.
