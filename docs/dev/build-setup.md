# Build Setup

Detailed build and environment notes for a source checkout.

The instructions below describe the native GCC/vcpkg environment used by the
`gcc-*` presets, with vcpkg roots at `/opt/vcpkg` and `/opt/vcpkg_asan`.
Its dependencies and compiler are built for the native host. The
[development container](container-build.md) provides an alternative with the
compiler and normal/ASan dependencies included. Both workflows use the vcpkg
revision recorded as `builtin-baseline` in [deps/vcpkg.json](../../deps/vcpkg.json).

## Development Requirements

- C++26-capable GCC and matching libstdc++; the currently verified environment
  uses a GCC 16 development snapshot
- CMake 3.25+ (the preset file uses schema version 6) and Ninja
- vcpkg (toolchain files expected at `/opt/vcpkg/`)
- Host tools: pkg-config and gfortran matching the selected GCC major version
- Dependencies: Protobuf, gRPC, Intel TBB, Boost, xxHash, spdlog, CLI11,
  LZ4, FAISS, glaze, GTL, GoogleTest, and Google Benchmark. The compiler
  supplies OpenMP; FastPFOR is fetched and uni-algo is vendored

The build uses `-march=native`; a release binary is intended for the machine
class on which its dependencies and Luxir itself were built.

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

Run Luxir's dependency setup before installing packages. It fetches pinned
FastPFOR and applies the required triplet and FAISS patches to the vcpkg root:

```bash
./deps/make_deps.sh /opt/vcpkg
```

Then install the current vcpkg set:

```bash
cd /opt/vcpkg
./vcpkg install boost-core boost-sort boost-thread boost-beast gtest benchmark \
  xxhash gtl protobuf grpc spdlog lz4 cli11 faiss glaze
```

For an existing installation, update the checkout to the recorded revision
while preserving Luxir's triplet/port patches, bootstrap vcpkg again, and rerun
`deps/apply_patches.sh` from the Luxir checkout. Run `./vcpkg upgrade` in the
vcpkg root to inspect the changes, then
`./vcpkg upgrade --no-dry-run --no-keep-going` to rebuild the affected packages.
Update the ASan root to the same revision and apply its instrumented triplet.
Perform the two dependency builds sequentially, then rebuild and test Luxir's
corresponding presets.

On Ubuntu, the remaining host packages include TBB, pkg-config, Ninja, and a
Fortran compiler whose major matches GCC:

```bash
sudo apt install libtbb-dev pkg-config ninja-build "gfortran-$(gcc -dumpversion)"
```

See [deps/README.txt](../../deps/README.txt) for why the local vcpkg patches and
the matching Fortran compiler are correctness requirements, not optional
tuning. A clean-machine setup can still require adjustment as upstream vcpkg
is updated or host tools differ; the recorded checkout does not yet pin the
complete build environment.

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
./build/gcc-debug/bin/luxir_test
```

Build the optimized server with:

```bash
cmake --preset gcc-release
cmake --build --preset gcc-release
./build/gcc-release/bin/luxir
```

Before committing memory-sensitive work, run the ASan build:

The ASan presets use a separate `/opt/vcpkg_asan` root whose dependencies must
also be built with ASan. Clone a second vcpkg tree at the recorded revision,
bootstrap it, rerun `deps/make_deps.sh /opt/vcpkg /opt/vcpkg_asan`, and install
the same package set there before configuring the preset. See
`deps/README.txt` for the required instrumented triplet.

```bash
cmake --preset gcc-debug-asan
cmake --build --preset gcc-debug-asan
ASAN_OPTIONS=detect_leaks=1 ./build/gcc-debug-asan/bin/luxir_test
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
