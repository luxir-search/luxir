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

The build uses `-march=native`; a release binary is intended for the machine
class on which its dependencies and Luxir itself were built.
TBB and its internal tbbmalloc allocator are static vcpkg libraries. Static
jemalloc replaces malloc/free and operator new/delete process-wide in every
build by default unless sanitizer flags are present (`LUXIR_JEMALLOC` then
defaults to `OFF`); explicitly enabling both is an error. The startup banner
reports the allocator version and effective configuration. The jemalloc overlay
embeds allocator defaults; see [dependency settings](../../deps/README.txt).
For Valgrind, configure a build with `-DLUXIR_JEMALLOC=OFF` or run with
`--soname-synonyms=somalloc=NONE`. OpenBLAS supplies C LAPACK, so neither a
Fortran compiler nor a Fortran runtime is required. Native builds retain shared
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

Configure sizes two limits from the machine with `tools/build-jobs`, which
counts physical cores and memory, honoring cgroup CPU and memory limits:

- `LUXIR_COMPILE_JOBS`: one compile per physical core, at most 16, and at most
  one per 3 GB of memory (4 GB with ASan) left after the link reservation.
- `LUXIR_LINK_JOBS`: at most two links, within a quarter of memory at 4 GB per
  link (6 GB with ASan).

Each limit is a Ninja pool, so a plain `cmake --build` needs no `-j`. Compiles
and links also take one of that many machine-wide slots (lock files under
`~/.cache/luxir/build-slots`, see `tools/build-slot`), so concurrent builds from
other worktrees, presets, or the development container share the limits rather
than each assuming the whole machine. A job waiting for a slot sleeps and uses no
memory. Override the limits with `-DLUXIR_COMPILE_JOBS=N` and
`-DLUXIR_LINK_JOBS=N` when configuring; set `-DLUXIR_BUILD_SLOT_DIR=` (empty) to
limit a build only by its own pools.

Shared state lives outside the checkout, so a new worktree reuses it:

- Native vcpkg dependencies are installed once under `/opt/vcpkg`, with
  vcpkg's default binary cache and downloads. vcpkg serializes installs that
  share its root. The install directories follow the manifest of the most recent
  `deps/make_deps.sh` run, so a branch that changes dependencies affects every
  native worktree once it is installed.
- ccache uses its normal cache directory. The launcher sets `CCACHE_BASEDIR` to
  the checkout and maps the checkout to `.` in debug information, so worktrees at
  different paths share objects: a new worktree's first build is then mostly
  cache hits plus links. A worktree's objects still miss wherever it changes a
  header they include. Each branch and preset adds its own objects, so size the
  cache for the worktrees in use at once (`max_size` in `ccache.conf`, e.g.
  50 GB for a few); a full cache evicts the objects the next worktree needs. Debug information therefore names sources
  relative to the checkout root: start GDB from the checkout root, or add it
  with `directory`. `__FILE__` is relative too, so tests locate data through
  `testDataPath()`, not their source path.
- `LUXIR_CACHE_DIR` (default `~/.cache/luxir`, or `$XDG_CACHE_HOME/luxir`) holds
  the build slots, the [ARM64](arm64-build.md) toolchain, vcpkg checkout,
  downloads, and binary packages, and the development container's ccache.
  Setup steps that write shared state take a lock, and vcpkg's binary cache
  is keyed by package ABI, so concurrent builds and different branches do not
  corrupt it.

Build directories and per-build installs (such as `build/arm64-deps`) stay in
each worktree, and must physically be there: ccache rewrites paths relative to
the real working directory, so a build directory outside the worktree (through
`-B` or a `build/` symlink) puts the worktree's name in every hashed path and
never hits another worktree's objects.

`tools/worktree add NAME [START]` creates a worktree on a new branch (from
`origin/main` by default) under `LUXIR_WORKTREE_ROOT` (default
`<main checkout>-wt`), with the submodule and FastPFOR fetched.
`tools/worktree remove NAME` deletes it with its builds, refusing uncommitted
or unpushed work without `-f`. Worktrees are cheap to recreate, so on a machine
whose source disk is short of space, point `LUXIR_WORKTREE_ROOT` at a larger
one: build directories are 10 to 20 GB per worktree. Prefer a directory that
systemd-tmpfiles does not age out (check `tmpfiles.d` for `/tmp` and
`/var/tmp`): aging deletes files untouched for its period even without a
reboot, including sources of a long-lived worktree.

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
