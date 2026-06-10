Building
--------

0) One-shot setup (idempotent; safe to re-run):

$ ./make_deps.sh        # defaults: /opt/vcpkg /opt/vcpkg_asan

   This fetches pinned simdcomp sources if absent (and restores the
   normally-vendored uni-algo if it is ever missing), applies Solux's local
   patches to the vcpkg roots and the simdcomp checkout, and builds the
   simdcomp static libs if missing. Run it BEFORE installing vcpkg packages,
   or reinstall any already-built package afterwards so it picks up the
   patched triplet/port (e.g. ./vcpkg remove faiss && ./vcpkg install faiss).

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
$ ./vcpkg install boost-core boost-sort boost-thread gtest benchmark xxhash gtl protobuf grpc spdlog lz4 cli11 faiss
$ ./vcpkg install robin-hood-hashing   #optional... see MapBM.cpp

Ubuntu:
```
sudo apt install libtbb-dev    #TODO - try the tbb in vcpkg now.
```

SIMDCompressionAndIntersection (simdcomp)
-----------------------------------------
make_deps.sh handles all of this; details for reference:

- Pinned to upstream commit b666a60c8fca18227d6532fb2d3b4d4dbc466cc9
  (lemire/SIMDCompressionAndIntersection master, 2020-12-11).
- patches/simdcomp.diff removes -D_GLIBCXX_DEBUG from the debug build (its
  debug-container ABI is incompatible with code built without the flag -
  things crash; the alternative of adding _GLIBCXX_DEBUG to every vcpkg
  debug library was considered and rejected) and un-statics a few functions
  Solux links directly.
- The libs CMake links live INSIDE the checkout (link_directories points at
  deps/simdcomp): libsimdcomp_a.a (release) and libsimdcomp_ad.a (DEBUG=1).
  Manual rebuild, should you need it:

  $ cd simdcomp
  $ make clean && make CXX=g++ CC=gcc DEBUG=1 libSIMDCompressionAndIntersection.a
  $ mv libSIMDCompressionAndIntersection.a libsimdcomp_ad.a
  $ make clean && make CXX=g++ CC=gcc libSIMDCompressionAndIntersection.a
  $ mv libSIMDCompressionAndIntersection.a libsimdcomp_a.a
  $ make clean

uni-algo
--------
Vendored in-repo (v1.2.0, Unicode 15.1.0; see uni-algo/VENDORED.txt) and
compiled by the main CMakeLists.txt (uni_algo target from src/data.cpp), so
nothing to do here. make_deps.sh can re-fetch the identical subset from the
upstream tag if the directory is ever removed.

TODO: automate / integrate simdcomp into the build system if we keep it.
