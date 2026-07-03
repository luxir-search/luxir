# Build Setup

Detailed build/environment notes. For the common build and test commands, see CLAUDE.md.

## Development Requirements

- C++23 compatible compiler (GCC or Clang)
- CMake 3.16+
- vcpkg (toolchain files expected at `/opt/vcpkg/`)
- Dependencies: Protobuf, gRPC, Intel TBB, Boost, xxHash, spdlog

Before installing vcpkg packages, run `deps/make_deps.sh` (idempotent): it
fetches the pinned simdcomp sources, applies Solux's local patches to the
vcpkg roots (triplet compile flags, FAISS SIMD opt level) and to simdcomp,
and builds the simdcomp static libs. Then install packages per
[deps/README.txt](../../deps/README.txt). `deps/apply_patches.sh` is the
patch-application step on its own.

The project uses CMake (Ninja generator) with vcpkg. ccache, a fast linker (mold), and a
precompiled header are auto-enabled by CMakeLists.txt. Each preset builds into its own
`build/<preset-name>/`, with binaries in `build/<preset-name>/bin/`.

Presets: gcc/clang x debug/release x +/-asan = 8 presets. Always use the **gcc** presets;
FAISS/OpenMP are only installed in the gcc vcpkg repos. The clang presets are disabled unless
`SOLUX_ENABLE_CLANG=1`.

## Fresh checkout / after `rm -rf build/`

Generated protobuf headers (`build/<preset>/protos/*.pb.h`) only exist after a build, and
IDE code insight (CLion uses clangd) parses the real sources that `#include` them. So on a
fresh checkout or after deleting `build/`: configure the preset (CLion does this automatically
on load), then **build once** to generate them, otherwise clangd reports the generated headers
as missing. Each preset you open in the IDE needs its own one-time build. The repo-root
`.clangd` makes clangd ignore the GCC precompiled header (it cannot read a `.gch`); the
gcc/ninja build still uses the PCH normally.

Delete any non-preset CLion "Debug" profile -- it has no vcpkg toolchain and will fail.
