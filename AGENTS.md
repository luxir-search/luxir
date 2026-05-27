## Project Overview

Solux is a high-performance hybrid search engine written in C++. It features a gRPC API, full-text indexing and searching, faceted search, and vector search.

See [docs/architecture.md](docs/architecture.md) for the component layout, data organization, and request flow.

## Build Commands

Use the **gcc** presets (FAISS is only in the gcc vcpkg repos). Each preset
builds into `build/<preset-name>/`, with binaries in `build/<preset-name>/bin/`. See
[docs/build-setup.md](docs/build-setup.md) for requirements and IDE/clangd setup.

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
generated `*.pb.h` headers must exist). See [docs/build-setup.md](docs/build-setup.md).

## Test Commands

```bash
# Run all tests (iteration build)
./build/gcc-debug/bin/solux_test

# Run specific test suite
./build/gcc-debug/bin/solux_test --gtest_filter="IndexWriterTest.*"

# Run benchmarks
./build/gcc-debug/bin/solux_test --bench

# Run the same suite under ASan before committing
./build/gcc-debug-asan/bin/solux_test
```

## Code Conventions

- C-style casts for numeric values
- members at top of C++ classes, no prefix / suffix
- do not use em dashes or other non-ascii (in source or prose)
- This is unreleased code, so NEVER worry about back compat.

## Writing Tests

- keep them short and maintainable
- never mock
- use test/test/CollectionHelper.h test/test/TestUtils.h and test/test/LocalReq.h

## Environment

- vcpkg toolchains located at `/opt/vcpkg/`, look there for source code for dependencies
