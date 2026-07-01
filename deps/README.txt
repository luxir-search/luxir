Building
--------

0) One-shot setup (idempotent; safe to re-run):

$ ./make_deps.sh        # defaults: /opt/vcpkg /opt/vcpkg_asan

   This fetches pinned FastPFOR sources if absent (and restores the
   normally-vendored uni-algo if it is ever missing) and applies Solux's local
   patches to the vcpkg roots (and FastPFOR, if any patches exist). FastPFOR
   itself is compiled by the main CMakeLists.txt, so there is no static-lib
   build step. Run it BEFORE installing vcpkg packages, or reinstall any
   already-built package afterwards so it picks up the patched triplet/port
   (e.g. ./vcpkg remove faiss && ./vcpkg install faiss).

   apply_patches.sh is the patch-application piece on its own; patches/ holds
   one file per change, and the script headers document what each one does.

   Why the triplet flags are required, not optional: the -march=native family
   of flags must match the project's own release CXXFLAGS. abseil's hash
   changes under these flags, so a mismatched protobuf builds a
   protobuf::Map<string,...> whose find() silently returns end() for entries
   that iteration sees (release builds only). The asan-root triplet also
   builds all dependencies with ASan; without that, newer gRPC/protobuf hit
   "use after poison" errors under the asan presets.

1) Install dependencies via vcpkg (repeat in the asan root for asan presets):

$ cd /opt/vcpkg
$ ./vcpkg install boost-core boost-sort boost-thread boost-beast gtest benchmark xxhash gtl protobuf grpc spdlog lz4 cli11 faiss glaze
$ ./vcpkg install robin-hood-hashing   #optional... see MapBM.cpp

Ubuntu:
```
sudo apt install libtbb-dev    #TODO - try the tbb in vcpkg now.
```

FastPFOR
--------
make_deps.sh clones this; details for reference:

- Pinned to tag v0.5.0 (fast-pack/FastPFOR), the release with ARM NEON
  support. Cloned into deps/FastPFOR (gitignored).
- Compiled by the main CMakeLists.txt: the `fastpfor` static-lib target builds
  just the bit-packing sources we use (bitpacking.cpp, simdbitpacking.cpp,
  simdunalignedbitpacking.cpp) with headers from deps/FastPFOR/headers. We do
  NOT compile streamvbyte.c / varintdecode.c / codecfactory.cpp (their C
  symbols are unused and were the only real clash risk). So there is no manual
  lib-build step.
- No local patches are needed at present. If one becomes necessary, drop it in
  patches/ as fastpfor*.diff and apply_patches.sh will apply it on the next run.

uni-algo
--------
Vendored in-repo (v1.2.0, Unicode 15.1.0; see uni-algo/VENDORED.txt) and
compiled by the main CMakeLists.txt (uni_algo target from src/data.cpp), so
nothing to do here. make_deps.sh can re-fetch the identical subset from the
upstream tag if the directory is ever removed; apply_patches.sh then re-applies
the local fix in patches/uni-algo-word-only-newline-leak.patch (the vendored
copy in git already has it applied - see the apply_patches.sh header for what
it fixes and why).
