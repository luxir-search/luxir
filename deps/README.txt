Building
--------

Native GCC setup: ../docs/dev/build-setup.md
Ubuntu 22.04 container: ../docs/dev/container-build.md

Both builds use vcpkg.json, its builtin-baseline, and the ports/ overlays.
No upstream vcpkg files need patching. After bootstrapping /opt/vcpkg at the
recorded revision, run from this directory:

  ./make_deps.sh

This fetches pinned sources and installs normal and ASan dependencies
sequentially into /opt/vcpkg/installed-native and installed-native-asan.
An optional first argument selects another vcpkg checkout; adjust the CMake
presets' toolchain and install paths accordingly. VCPKG_MAX_CONCURRENCY defaults
to 12. Sources and binary caches are shared; completed build trees are cleaned.

Triplets
--------

triplets/x64-linux-luxir-native*.cmake use -march=native -mtune=native for C and
C++. The setup script records the compilers' effective CPU options in the
binary-cache ABI, preventing reuse across incompatible native CPU settings.
triplets/x64-linux-luxir-v2*.cmake provide the container's x86-64-v2 baseline.
The common fragments select static libraries and disable the IPO configuration
unsupported by static oneTBB. ASan variants instrument all target dependencies;
host code generators run unsanitized. Native ASan uses the Release-only
x64-linux-luxir-native-host triplet for host packages, avoiding another copy of
the normal Debug libraries. Normal native builds reuse their target triplet for
host tools; container ASan uses x64-linux-luxir-v2 for host tools.
ASan Debug libraries retain full debug information. Optimized ASan libraries
use -g1 for stack traces and line numbers, avoiding expensive GCC tracking of
optimized local variables.

Use matching CPU flags for Luxir and native dependencies. Some inline library
implementations select behavior based on compiler feature macros; historically,
mismatched abseil/protobuf hashing flags broke protobuf::Map lookups. For a
controlled compilation experiment, audit those interfaces before changing one
side independently.

Libraries
---------

- TBB and tbbmalloc come from the pinned vcpkg port. Luxir retains the static
  allocator entry points needed by TBB. Application malloc/free remain glibc's.
- The FAISS overlay enables runtime SIMD dispatch (FAISS_OPT_LEVEL=dd) and
  checks the CPU features required by each dispatched kernel.
- OpenBLAS includes C LAPACK with 32-bit LAPACK integers. No Fortran compiler
  or runtime is needed. BLAS is single-threaded with locking for concurrent
  callers; CPU-specific kernels are selected at runtime. ASan disables the
  AVX-512 OpenBLAS kernels whose inline assembly GCC cannot instrument.
  The overlay preserves caller CPU flags for common code and C LAPACK; upstream
  otherwise strips -march=native as a workaround for older GCC versions.
- The lapack overlay directs FAISS to the LAPACK routines inside OpenBLAS.

For dependency changes, rerun make_deps.sh, then rebuild and test Luxir. Old
classic installs under /opt/vcpkg/installed and /opt/vcpkg_asan are not used by
the presets. On migration, configure each native preset once with --fresh to
discard cached package locations.

The Release-only native host change takes effect on the next make_deps.sh run,
which migrates the ASan install and can rebuild affected dependencies. Existing
build trees can still use the current installation until then. For an ASan
configure before migration, override VCPKG_HOST_TRIPLET=x64-linux-luxir-native.
After migration, configure native ASan presets with --fresh to clear cached
tool paths. See ../docs/dev/build-setup.md for the commands.

Vendored sources
----------------

fetch_sources.sh clones FastPFOR at v0.5.0 (fast-pack/FastPFOR) if absent. The
main CMakeLists.txt builds its selected bit-packing and streamvbyte sources.

uni-algo is vendored at v1.2.0 (Unicode 15.1.0). fetch_sources.sh restores it
if absent and applies patches/uni-algo-word-only-newline-leak.patch. The fix
resets the word property accumulator at newline breaks, preventing word_only
from emitting newline runs after a word. Word boundaries are unchanged.
