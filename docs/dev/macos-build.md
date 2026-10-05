# Native macOS build

The default build uses Apple's compiler and system libc++, plus installed
Homebrew libraries. No vcpkg setup, preset, custom compiler, or environment
variables are required. Install the Apple Command Line Tools and CMake first.

From the repository root:

```bash
cmake -B build
cd build
make -j
```

CMake discovers Homebrew's installation prefix and reports missing libraries
together, including the brew install command. It never installs system packages.
The full dependency set is:

```bash
brew install xxhash boost tbb spdlog cli11 lz4 faiss protobuf grpc glaze gtl googletest google-benchmark libomp howard-hinnant-date
```

libomp supplies OpenMP for AppleClang. howard-hinnant-date supplies IANA time
zones because Apple's libc++ does not provide the standard time-zone database.
The compiler must support the C++26 mode and the standard-library features used
by Luxir. The current AppleClang 17 installation passes the initial feature probe;
the complete build must be validated against the installed library versions.

CMake uses local FastPFOR and hpp-proto sources when present, otherwise downloads
the pinned revisions into the build directory. uni-algo and is_utf8 are vendored.

Run the tests after building:

```bash
./bin/luxir_test --gtest_brief=1 --gtest_print_time=0
```
