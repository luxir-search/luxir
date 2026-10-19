# Linux ARM64 release build

`tools/build-arm64` cross-compiles Luxir and its test binary on a Linux x86-64
host for the generic ARMv8-A baseline (`armv8-a`, with NEON). The outputs are
`build/arm64-release/bin/luxir` and `build/arm64-release/bin/luxir_test`.

## Prerequisites

Use the [native development environment](build-setup.md), including GCC 16,
Ninja, Git, Python 3, make, curl, tar, xz, zip/unzip, pkg-config, and the
autotools required by vcpkg. Initialize `deps/hpp-proto` with
`git submodule update --init`. Internet access is required for pinned tools
and dependency archives.

The workflow downloads Compiler Explorer's GCC 16.1.0 AArch64 cross compiler,
checks its SHA-256 digest, and uses its bundled Linux sysroot and compiler
runtimes. The script uses vcpkg's downloaded CMake 4.4.3;
`LUXIR_CMAKE` can override its path.

```bash
./tools/build-arm64
```

The first run builds all dependencies. Set `LUXIR_BUILD_JOBS=4` on machines
with less memory. Stage logs are in `build/arm64-logs/`, and errors include
the failed log's tail. Downloaded tools, sources, dependencies, and compiler
caches stay under `build/`.

The native hpp-proto generator uses `g++-16`; `CXX` can override it with an
equivalent C++26-capable GCC executable. vcpkg selects the host compiler for
its own build tools independently of `CC`/`CXX`.

## Runtime compatibility

The pinned sysroot supplies glibc 2.41. Deploy on a Linux ARM64 system with
compatible glibc symbol versions and time-zone data. Generic CPU support does
not imply compatibility with older glibc releases or musl-based systems.
Product libraries, libstdc++, libgcc, and OpenMP are linked statically; glibc
and libm remain dynamic.
Cross builds use the linker supplied by the toolchain. Native builds retain
automatic selection of a faster host linker.

The vcpkg manifest and overlay ports are shared with native builds. The ARM64
triplet builds Release libraries only; native protobuf/gRPC tools have a
separate host triplet. `protoc-gen-hpp` is compiled with the native compiler.
OpenBLAS uses an ARMV8 baseline with runtime dispatch to supported NEON/SVE
kernels. FAISS also selects its ARM NEON/SVE implementation at runtime.

## Validation

If `qemu-aarch64` is installed, the build script executes its toolchain and
LAPACK smoke tests and `luxir --version` with a Cortex-A53 CPU model to check
the ARMv8-A baseline. Set `LUXIR_QEMU_AARCH64` to use an emulator at another path.

Run the tests on a compatible ARM64 Linux host or under QEMU:

```bash
arm64_sysroot="$PWD/build/arm64-toolchain/compiler/gcc-16.1.0/aarch64-unknown-linux-gnu/aarch64-unknown-linux-gnu/sysroot"
qemu-aarch64 -cpu cortex-a53 -L "$arm64_sysroot" \
  build/arm64-release/bin/luxir --version
qemu-aarch64 -cpu cortex-a53 -L "$arm64_sysroot" \
  build/arm64-release/bin/luxir_test --gtest_brief=1 --gtest_print_time=0
```

QEMU user-mode checks the executable against the target CPU and libraries.
Kernel-specific tests still need a native ARM64 host: for example,
[QEMU 8.2 ignores huge-page advice](https://github.com/qemu/qemu/blob/v8.2.2/linux-user/mmap.c#L908-L971),
so allocator tests that inspect `/proc/self/smaps` cannot validate those hints
under that emulator.
