## Project Overview

Solux is a high-performance hybrid search engine written in C++. It features a gRPC API, full-text indexing and searching, faceted search, and vector search.

See [docs/dev/codebase-map.md](docs/dev/codebase-map.md) for the component/class layout, data organization, and request flow, and [docs/design/architecture.md](docs/design/architecture.md) for the design overview and rationale. [docs/README.md](docs/README.md) maps the documentation tree (guide/ = user, design/ = architecture+decisions, dev/ = contributor).

## Build Commands

Use the **gcc** presets (FAISS is only in the gcc vcpkg repos). Each preset
builds into `build/<preset-name>/`, with binaries in `build/<preset-name>/bin/`. See
[docs/dev/build-setup.md](docs/dev/build-setup.md) for requirements and IDE/clangd setup.

### Iterate (default): non-ASan, fastest edit-build-test

```bash
cmake --preset gcc-debug          # configure build/gcc-debug/ (first time only)
cmake --build --preset gcc-debug  # binaries in build/gcc-debug/bin/
```

### Memory checks: ASan (run before committing, or when debugging a crash/UB)

```bash
cmake --preset gcc-debug-asan
cmake --build --preset gcc-debug-asan   # binaries in build/gcc-debug-asan/bin/
```

Fresh checkout or after `rm -rf build/`: build once before IDE code-insight works (the
generated `*.pb.h` headers must exist). See [docs/dev/build-setup.md](docs/dev/build-setup.md).

## Test Commands

```bash
# Run all tests (iteration build)
./build/gcc-debug/bin/solux_test

# Run specific test suite
./build/gcc-debug/bin/solux_test --gtest_filter="IndexWriterTest.*"

# Run benchmarks (NOTE: builds production-scale corpora - slow setup, use
# gcc-release and memory caps (ulimit -v 32000000) for real measurements.
# For quick iteration/coverage use the small-corpus unit-test mode instead:
#   ./build/gcc-debug/bin/solux_test --gtest_filter="Benchmarks.all"
./build/gcc-debug/bin/solux_test --bench

# Run all benchmarks except the slow vector ones (HNSW/IVFPQ builds dominate
# wall-clock). Negative google-benchmark filter excludes the BM_Vector* family:
./build/gcc-release/bin/solux_test --bench --benchmark_filter='-BM_Vector'

# Run the same suite under ASan before committing
./build/gcc-debug-asan/bin/solux_test
```

## Code Conventions

- C-style casts for numeric values
- members at top of C++ classes, no prefix / suffix
- do not use em dashes or other non-ascii (in source or prose)
- This is unreleased code, so NEVER worry about back compat.
- Allocate engine objects that need a destructor (search ops, Query::Context,
  request/response wrappers) with solux::arenaCreate<T> (src/solux/util/proto.h),
  not protobuf's Arena::Create<T>. arenaCreate constructs first and registers the
  destructor only on success, so a throwing ctor is safe. Raw Arena::Create<T>
  registers the cleanup node before placement-new and runs ~T() on
  half-constructed memory if the ctor throws -> crash at arena reset.

## Writing Tests

- keep them short and maintainable
- never mock
- use test/test/CollectionHelper.h test/test/TestUtils.h and test/test/LocalReq.h

## Environment

- vcpkg toolchains located at `/opt/vcpkg/`, look there for source code for dependencies
