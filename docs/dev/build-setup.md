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

For a Linux ARM64 executable built on an x86-64 host,
use the [ARM64 cross-build workflow](arm64-build.md).

For threaded OpenBLAS, AMD AOCL, Arm Performance Libraries, and vector-build
thread budgets, see [CPU BLAS backends](blas.md).

## Development Requirements

- C++26-capable GCC and matching libstdc++; the currently verified environment
  uses a GCC 16 development snapshot
- CMake 3.25+ (the preset file uses schema version 6) and Ninja
- vcpkg (toolchain files expected at `/opt/vcpkg/`)
- Host tools: pkg-config, Python 3, curl, zip, unzip, and tar
- Dependencies: Protobuf, gRPC, Intel TBB, Boost, xxHash, spdlog, CLI11,
  LZ4, Zstandard, FAISS, glaze, GTL, jemalloc, GoogleTest, and Google
  Benchmark. The compiler supplies OpenMP; FastPFOR is fetched and uni-algo is
  vendored

The build targets the host CPU (`-march=native`, or the host's `-mcpu` on Apple
silicon); a release binary is intended for the machine class on which its
dependencies and Luxir itself were built.
TBB and its internal tbbmalloc allocator are static vcpkg libraries. Static
jemalloc replaces malloc/free and operator new/delete process-wide in every
build by default unless sanitizer flags are present (`LUXIR_JEMALLOC` then
defaults to `OFF`); explicitly enabling both is an error. The startup banner
reports the allocator version and effective configuration. The jemalloc overlay
embeds allocator defaults; see [dependency settings](../../deps/README.txt).
For Valgrind, configure a build with `-DLUXIR_JEMALLOC=OFF` or run with
`--soname-synonyms=somalloc=NONE`. OpenBLAS supplies C LAPACK (Accelerate does
on macOS), so neither a Fortran compiler nor a Fortran runtime is required. Native builds retain shared
GCC runtimes; the container's static runtime setting is a separate packaging
choice.

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

On macOS, install the Command Line Tools (`xcode-select --install`), then the
compiler and build tools from Homebrew; [macOS](#macos-apple-silicon) lists the
differences from Linux:

```bash
brew install gcc cmake ninja pkg-config ccache
```

Build both native dependency variants:

```bash
./deps/make_deps.sh
```

The script fetches pinned FastPFOR sources, checks the vcpkg revision, and installs
the manifest sequentially with `x64-linux-luxir-native` and
`x64-linux-luxir-native-asan`. Dependency build jobs default to what the cores
and memory allow (`tools/build-jobs`); `VCPKG_MAX_CONCURRENCY` overrides
that. Both variants include
Debug and Release libraries. ASan uses unsanitized host tools from the
Release-only `x64-linux-luxir-native-host` triplet, avoiding duplicated normal
Debug libraries in the ASan install. Normal native builds reuse their target
triplet for host tools.

The script fingerprints the selected GCC compilers' effective native CPU options
for vcpkg's binary cache, so a package built on one CPU is not reused on another
with different native settings. Use this script when updating dependencies;
direct native-triplet installs require that fingerprint. `CC` and `CXX` default
to `gcc-16` and `g++-16`; the native presets pin those same compiler names.
If overriding them, select the same compilers when configuring Luxir.
Existing build directories need a fresh configure (`cmake --preset <name> --fresh`)
because CMake cannot switch compilers in an existing cache. Reapply local CMake
options when refreshing.

After changing the manifest or overlays, rerun the script. To update vcpkg itself,
change the manifest baseline, check out that revision in `/opt/vcpkg`, bootstrap
again, then rerun the script. No changes to upstream ports or triplets are needed.

### macOS (Apple silicon)

The same presets and setup script work with Homebrew GCC. On macOS:

- Dependencies use the `arm64-osx-luxir-m1*` triplets: a fixed `apple-m1`
  and macOS 14 baseline instead of the host CPU, so one install serves local
  builds and binaries for other Macs. Later Apple cores add only BF16 and int8
  matrix-multiply instructions, and no dependency header tests for them, so
  Luxir built for a newer chip sees the same inline dependency code. No CPU
  fingerprint is needed.
- `LUXIR_CPU_TARGET=native` resolves to the host chip (`apple-m5` and so on,
  from `sysctl`), since GCC's `-mcpu=native` reports every Apple core as
  `apple-m1`. Configure with `-DLUXIR_CPU_TARGET=apple-m1` for a binary that
  runs on any Apple silicon Mac.
- jemalloc is not yet supported; `LUXIR_JEMALLOC` defaults to `OFF`.
- ASan works, but GCC has no LeakSanitizer there; `detect_leaks=1` is
  accepted and finds nothing.
- FAISS uses Apple's Accelerate for BLAS and LAPACK instead of OpenBLAS. Its
  matrix unit runs SGEMM at about 15 times the rate of single-threaded
  OpenBLAS on an M5. Accelerate runs its own threads; `VECLIB_MAXIMUM_THREADS`
  caps them.
- FAISS is built without its Metal GPU backend, which needs Xcode's `metal`
  compiler.

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
The exact host compiler build and system packages are not pinned by this workflow.

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
(mold 2.31.0 or newer, else lld or gold), and a precompiled header are used
when available. Each preset builds
into `build/<preset-name>/`, with binaries in `build/<preset-name>/bin/`.

The presets are GCC-only. Clang presets existed but were never usable: FAISS and
OpenMP are only set up in the GCC vcpkg roots, so configuring one failed at
`find_package(OpenMP)`. Building for Clang means preparing a Clang vcpkg root
with those dependencies first, then adding the preset back.

## Parallelism and shared caches

Configure sizes `LUXIR_COMPILE_JOBS` from the machine with `tools/build-jobs`,
which counts physical cores and memory, honoring cgroup CPU and memory limits:
one compile per physical core, at most 16, and at most one per 3 GB of memory
(4 GB with ASan).

The limit is a Ninja pool, so a plain `cmake --build` needs no `-j`. Compiles
also take one of that many machine-wide slots (lock files under
`~/.cache/luxir/build-slots`, see `tools/build-slot`), so concurrent builds from
other worktrees, presets, or the development container share the limit rather
than each assuming the whole machine. A compile waiting for a slot sleeps and
uses no memory. Override the limit with `-DLUXIR_COMPILE_JOBS=N` when
configuring; set `-DLUXIR_BUILD_SLOT_DIR=` (empty) to limit a build only by its
own pool.

Shared state lives outside the checkout, so a new worktree reuses it:

- Native vcpkg dependencies are installed once under `/opt/vcpkg`, with
  vcpkg's default binary cache and downloads. vcpkg serializes installs that
  share its root. The install directories follow the manifest of the most recent
  `deps/make_deps.sh` run, so a branch that changes dependencies affects every
  native worktree once it is installed.
- ccache uses its normal cache directory. Objects are shared between worktrees
  at different paths only when every checkout involved, including the main one,
  is configured with `-DLUXIR_CCACHE_RELATIVE_PATHS=ON`. That sets
  `CCACHE_BASEDIR` to the checkout and maps the checkout to `.` in debug
  information, with two costs: compiler diagnostics and `__FILE__` name sources
  relative to the build directory (`../../src/...`), and debuggers resolve
  sources from the checkout root, so start GDB there or add it with
  `directory`. Size the cache for several builds (one debug build is over
  4 GB of objects), e.g. `ccache -M 20G`.
- `LUXIR_CACHE_DIR` (default `~/.cache/luxir`, or `$XDG_CACHE_HOME/luxir`) holds
  the build slots, the [ARM64](arm64-build.md) toolchain, vcpkg checkout,
  downloads, and binary packages, and the development container's ccache.
  Setup steps that write shared state take a lock, and vcpkg's binary cache
  is keyed by package ABI, so concurrent builds and different branches do not
  corrupt it.

Build directories and per-build installs (such as `build/arm64-deps`) stay in
each worktree.

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
`gcc-debug-asan` compiles Luxir with `-g1`: backtraces keep functions, inlined
frames, and line numbers, but a debugger cannot show local variables. To
inspect locals under ASan, configure with
`-DCMAKE_CXX_FLAGS_DEBUG="-g -Og -ftrivial-auto-var-init=pattern"`.

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
./build/gcc-release/bin/luxir_test --gtest_filter='Benchmarks.all'
```

Debug builds, including ASan, leave the benchmarks out of `luxir_test` to keep
the edit-build-test loop short. Configure with `-DLUXIR_TEST_BENCHMARKS=ON` to
include them.

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

IANA time zones are read at runtime from the system zoneinfo database
(`$TZDIR`, else `/usr/share/zoneinfo`): zone and link names and the version
come from its `tzdata.zi`, and the rules from the compiled TZif files that the
C library also uses. Without it, the service degrades to UTC and fixed-offset
zones only. The loaded version is logged at startup. Requests produce identical
civil-time boundaries on machines with identical zoneinfo trees, whatever the
compiler or C++ runtime.

See [Operating Luxir](../guide/operations.md) for persistence, ports, security,
and resource controls after the binary is built.
