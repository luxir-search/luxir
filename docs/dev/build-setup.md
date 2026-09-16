# Build Setup

Detailed build and environment notes for a source checkout.

The instructions below describe the native GCC/vcpkg environment used by the
`gcc-*` presets, with one vcpkg checkout at `/opt/vcpkg`. Its dependencies and
compiler are built for the native host. The
[development container](container-build.md) provides an alternative with the
compiler and normal/ASan dependencies included. Both workflows use the vcpkg
revision recorded as `builtin-baseline` in [deps/vcpkg.json](../../deps/vcpkg.json),
the same manifest, and the same overlay ports. Native dependencies are installed
in `/opt/vcpkg/installed-native` and `/opt/vcpkg/installed-native-asan`.

## Development Requirements

- C++26-capable GCC and matching libstdc++; the currently verified environment
  uses a GCC 16 development snapshot
- CMake 3.25+ (the preset file uses schema version 6) and Ninja
- vcpkg (toolchain files expected at `/opt/vcpkg/`)
- Host tools: pkg-config, Python 3, curl, zip, unzip, and tar
- Dependencies: Protobuf, gRPC, Intel TBB, Boost, xxHash, spdlog, CLI11,
  LZ4, FAISS, glaze, GTL, GoogleTest, and Google Benchmark. The compiler
  supplies OpenMP; FastPFOR is fetched and uni-algo is vendored

The build uses `-march=native`; a release binary is intended for the machine
class on which its dependencies and Luxir itself were built.
TBB and its internal tbbmalloc allocator are static vcpkg libraries. Application
malloc/free remain glibc's. OpenBLAS supplies C LAPACK, so neither a Fortran
compiler nor a Fortran runtime is required. Native builds retain shared GCC
runtimes; the container's static runtime setting is a separate packaging choice.

## Prepare a checkout

Initialize the code-generation submodule first:

```bash
git submodule update --init
```

Install or clone vcpkg at `/opt/vcpkg` and bootstrap it. The absolute path is
part of the current GCC presets:

```bash
sudo git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg
sudo chown -R "$(id -un):$(id -gn)" /opt/vcpkg
luxir_vcpkg_revision=$(python3 -c 'import json; print(json.load(open("deps/vcpkg.json"))["builtin-baseline"])')
git -C /opt/vcpkg checkout --detach "$luxir_vcpkg_revision"
/opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
```

Install the host tools (in addition to GCC/G++, CMake, and Ninja):

```bash
sudo apt install pkg-config python3 curl zip unzip tar
```

Build both native dependency variants:

```bash
./deps/make_deps.sh
```

The script fetches pinned FastPFOR sources, checks the vcpkg revision, and installs
the manifest sequentially with `x64-linux-luxir-native` and
`x64-linux-luxir-native-asan`. It defaults to 12 dependency build jobs; override
`VCPKG_MAX_CONCURRENCY` for the machine's memory budget. Both variants include
Debug and Release libraries. ASan uses unsanitized host tools from the
Release-only `x64-linux-luxir-native-host` triplet, avoiding duplicated normal
Debug libraries in the ASan install. Normal native builds reuse their target
triplet for host tools.

The script fingerprints the selected GCC compilers' effective native CPU options
for vcpkg's binary cache, so a package built on one CPU is not reused on another
with different native settings. Use this script when updating dependencies;
direct native-triplet installs require that fingerprint. `CC` and `CXX` default
to `gcc` and `g++`, matching the presets. If overriding them, select the same
compilers when configuring Luxir.

After changing the manifest or overlays, rerun the script. To update vcpkg itself,
change the manifest baseline, check out that revision in `/opt/vcpkg`, bootstrap
again, then rerun the script. No changes to upstream ports or triplets are needed.

When migrating an existing build from the old classic-mode dependencies, reset
the CMake caches so cached package paths cannot retain system TBB or the old
Fortran LAPACK provider:

```bash
cmake --fresh --preset gcc-debug
cmake --fresh --preset gcc-release
cmake --fresh --preset gcc-debug-asan
```

Reapply local CMake options such as `-DLUXIR_LOCAL_TARGETS=ON` when refreshing.
The new installs do not modify `/opt/vcpkg/installed` or `/opt/vcpkg_asan`.
Those legacy installations can be removed after migrating their consumers.
See [deps/README.txt](../../deps/README.txt) for the dependency configuration.
The host compiler and system packages are not pinned by this workflow.

### Deferring the Release-only host migration

Changing the scripts and presets does not rebuild or remove installed packages.
Existing native build trees can keep using their current dependencies. If the
ASan install still contains host tools under `x64-linux-luxir-native`, use this
temporary override when configuring before the next dependency setup run:

```bash
cmake --preset gcc-debug-asan -DVCPKG_HOST_TRIPLET=x64-linux-luxir-native
```

Use the same override for `gcc-release-asan` if needed. Normal native presets
need no override. When ready to migrate:

```bash
./deps/make_deps.sh
cmake --fresh --preset gcc-debug-asan
```

The setup run replaces the ASan install's old host packages with Release-only
host packages and can rebuild affected dependencies. Refresh any other native
ASan build trees too, reapplying local CMake options. This clears cached paths
to the old host tools. The disk-space saving occurs during this migration.

The project uses CMake (Ninja generator) with vcpkg. ccache, a fast linker
(mold), and a precompiled header are used when available. Each preset builds
into `build/<preset-name>/`, with binaries in `build/<preset-name>/bin/`.

The presets are GCC-only. Clang presets existed but were never usable: FAISS and
OpenMP are only set up in the GCC vcpkg roots, so configuring one failed at
`find_package(OpenMP)`. Building for Clang means preparing a Clang vcpkg root
with those dependencies first, then adding the preset back.

## Fresh checkout / after `rm -rf build/`

Generated API and descriptor headers only exist after a build, and IDE code
insight parses sources that include them. On a fresh checkout or after deleting
`build/`, configure and build once before treating missing generated headers as
an IDE problem. Each preset needs its own initial generation. The repo-root
`.clangd` makes clangd ignore the GCC precompiled header; the GCC/Ninja build
still uses the PCH normally.

Delete any non-preset CLion "Debug" profile -- it has no vcpkg toolchain and will fail.

## Build and test

When working with the native toolchain, use the non-ASan debug build for iteration:

```bash
cmake --preset gcc-debug
cmake --build --preset gcc-debug
./build/gcc-debug/bin/luxir_test --gtest_brief=1 --gtest_print_time=0
```

Build the optimized server with:

```bash
cmake --preset gcc-release
cmake --build --preset gcc-release
./build/gcc-release/bin/luxir
```

Before committing memory-sensitive work, run the ASan build:

The ASan presets use the instrumented dependencies in
`/opt/vcpkg/installed-native-asan`, built by the same setup script.

```bash
cmake --preset gcc-debug-asan
cmake --build --preset gcc-debug-asan
ASAN_OPTIONS=detect_leaks=1 ./build/gcc-debug-asan/bin/luxir_test --gtest_brief=1 --gtest_print_time=0
```

The binaries for every preset are under `build/<preset>/bin/`.

## Measure on your workload

There are no published performance numbers yet. Hardware, corpus, query mix,
vector model, and requested exactness all change the result, so evaluate the
release build on representative inputs.

The built-in benchmark mode creates production-scale corpora and can take
substantial setup time and memory:

```bash
ulimit -v 32000000
./build/gcc-release/bin/luxir_test --bench --benchmark_filter='-BM_Vector'
```

The negative filter excludes the slow vector families. For a quick path/code
coverage pass over the small unit-test corpus, use:

```bash
./build/gcc-debug/bin/luxir_test --gtest_filter='Benchmarks.all'
```

Test runs default to `--effort=1`. Scalable fuzz loops and benchmark unit
corpora can derive linear work with `LuxirTest::scaleTestWork()` or scale each
side of a multidimensional space with `LuxirTest::scaleTestDimension()`. Raise
the budget for a broader pass, for example:

```bash
./build/gcc-debug-asan/bin/luxir_test --effort=4
```

Effort 4 aims for roughly four times the total scalable work, including when
multiple loop bounds must each grow by a root of the effort. Explicit
`--bench` mode continues to use the production benchmark parameters.

Treat those as harnesses, not published comparative results.

## Time-zone database at runtime

`std::chrono` time-zone support requires the system zoneinfo database to be
present at runtime. Without it, the service degrades to UTC and fixed-offset
zones only. The loaded tzdb version is logged at startup. Keep the zoneinfo
data and the C++ runtime consistent across a fleet: identical tzdb version
strings do not guarantee identical transition decoding across different
parser implementations.

See [Operating Luxir](../guide/operations.md) for persistence, ports, security,
and resource controls after the binary is built.
