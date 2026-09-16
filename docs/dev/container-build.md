# Container development build

The development image uses Ubuntu 22.04 (glibc 2.35), GCC 16.2, CMake 4.4.3,
Ninja, ccache, and one pinned vcpkg checkout. It contains normal and
AddressSanitizer dependencies, each with Debug and Release libraries.
Linux x86-64 is the current target.

The two installs live at `/opt/vcpkg/installed` and `/opt/vcpkg/installed-asan`
in the same checkout. They share source pins and the build's binary cache.
Separate install directories prevent vcpkg's manifest reconciliation from
removing one variant's packages while installing the other.

Editors supporting the Dev Container specification can also open the checkout
using `.devcontainer/devcontainer.json`. Select `container-debug` as the editor's
CMake preset and build once to generate the API headers used by code insight.
Inside the dev container, run CMake and test binaries directly; the
`tools/dev-container` wrapper is for commands launched from the host.

## Build the image

Install Docker with BuildKit and confirm that `docker info` works in your current
login. If you just joined the `docker` group, start a new login session to activate
that membership. Then run from a source checkout:

```bash
git submodule update --init
./deps/fetch_sources.sh
./tools/dev-container build-image
```

The first image build compiles dependencies and takes considerably longer than
an ordinary Luxir build. BuildKit caches downloads, packages, and image layers.
It defaults to 12 dependency build jobs; reduce that on a smaller machine:

```bash
./tools/dev-container build-image --build-arg BUILD_JOBS=2
```

The image build checks C++26, exceptions, chrono timezone data, static OpenMP,
and a symbolized deliberate ASan error on Ubuntu 22.04. It also checks all ten
LAPACK entry points referenced by FAISS, including numerical reconstruction.

Compiler and CMake archives are verified against recorded checksums, and the
Ubuntu base is pinned by image digest. The compiler is a prebuilt Compiler
Explorer archive. Ubuntu packages receive current security updates when that
apt layer is rebuilt; the Dockerfile is not a bit-for-bit reproducibility claim.
The vcpkg source pin is `builtin-baseline` in [the manifest](../../deps/vcpkg.json).
The FAISS and OpenBLAS overlays retain the upstream port structure and patches;
the copied port files use [vcpkg's MIT license](../../deps/ports/LICENSE.vcpkg).

## Build and test Luxir

```bash
./tools/dev-container cmake --preset container-debug
./tools/dev-container cmake --build --preset container-debug
./tools/dev-container build/container-debug/bin/luxir_test --gtest_brief=1 --gtest_print_time=0

./tools/dev-container cmake --preset container-release
./tools/dev-container cmake --build --preset container-release
./tools/dev-container build/container-release/bin/luxir_test --gtest_brief=1 --gtest_print_time=0

./tools/dev-container cmake --preset container-asan
./tools/dev-container cmake --build --preset container-asan
./tools/dev-container env ASAN_OPTIONS=detect_leaks=1:allow_addr2line=1 build/container-asan/bin/luxir_test --gtest_brief=1 --gtest_print_time=0
```

Run builds sequentially. The compile job limit accounts for physical memory,
container memory limits, and ASan's larger compiler processes. Override it with
`-DLUXIR_COMPILE_JOBS=N` when configuring if necessary.

CMake downloads checksum-verified book and Unicode fixtures into each build's
`test/data/` directory, so they survive between container commands. If a download
is unavailable, configure reports it and the affected tests/benchmarks skip.
The image also includes the word list used by the dictionary benchmarks.

With no arguments, `tools/dev-container` opens a shell. It mounts the checkout
at `/workspace/luxir`, runs as your UID/GID, and puts its home and ccache under
`build/container-cache/`. Native build directories and presets remain usable.
The wrapper grants `SYS_PTRACE` for debugging within the container. It honors
Docker's `DOCKER_CONTEXT`, `CONTAINER_ENGINE` (default `docker`), and
`LUXIR_DEV_IMAGE` (default `luxir-dev:jammy-gcc16`).
With a remote Docker context, the same checkout must be shared at the same
absolute path on the daemon host so the bind mount resolves to your source.

## CPU targets and runtime libraries

The container presets compile Luxir at `x86-64-v2`, including vendored C/C++
sources. Change only the engine target for a separate experiment:

```bash
./tools/dev-container cmake --preset container-release -B build/container-v3 -DLUXIR_CPU_TARGET=x86-64-v3
./tools/dev-container cmake --build build/container-v3
```

Dependencies stay at v2. FAISS and OpenBLAS contain runtime-selected kernels.
The OpenBLAS overlay includes C LAPACK with 32-bit LAPACK integers and keeps
BLAS single-threaded, with locking for concurrent callers. Its ASan build
excludes AVX-512 OpenBLAS kernels because GCC cannot instrument their inline
assembly; FAISS retains its own AVX-512 kernels.

Product libraries and GCC runtimes are static; glibc and libm remain dynamic.
Static TBB retains tbbmalloc for TBB's own allocation. Application malloc/free
remain glibc's; the image does not select a global replacement allocator.
ASan has its usual diagnostic allocator interception. No system TBB or Fortran
runtime is needed by the resulting Luxir executable.

Debug and ASan builds contain debug information and frame pointers, including
dependency debug information. Release retains full Luxir debug information so
packaging can later split symbols from the exact optimized executable; its
dependency libraries use vcpkg's normal Release symbol settings.
ASan Release dependency libraries use `-g1` for stack traces and line numbers;
ASan Debug libraries retain full variable information. Dependency source trees
are omitted from the image; retrieve the matching sources when
stepping into them in GDB.

Release packaging, final CPU-tier selection, and execution checks on older
CPUs are separate work.

## Share an already-built environment

Others can use the same image without compiling the dependencies:

```bash
docker save luxir-dev:jammy-gcc16 | gzip > luxir-dev-jammy-gcc16.tar.gz
# On another machine:
gunzip -c luxir-dev-jammy-gcc16.tar.gz | docker load
```

They still initialize the source checkout and run the same wrapper commands.
An image can also be stored in a registry; set `LUXIR_DEV_IMAGE` to its pinned
registry digest. No registry publication is part of the local build.

When updating dependencies, change the manifest baseline, review the overlay
ports against that checkout, and rebuild/test the image. Native builds share
these ports and the manifest, using native CPU triplets and separate install
directories. `deps/make_deps.sh` builds those native variants on the host; the
Dockerfile builds the portable variants for this image.
