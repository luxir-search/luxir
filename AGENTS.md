## Project Overview

Luxir is a high-performance hybrid search engine written in C++. It features a gRPC API, full-text indexing and searching, faceted search, and vector search.

See [docs/dev/codebase-map.md](docs/dev/codebase-map.md) for the component/class layout, data organization, and request flow, and [docs/design/architecture.md](docs/design/architecture.md) for the design overview and rationale. [docs/README.md](docs/README.md) maps the documentation tree (guide/ = user, design/ = architecture+decisions, dev/ = contributor).

## Build Commands

Use the workflow configured for the checkout:

- Native GCC/vcpkg: use the `gcc-*` presets; see
  [docs/dev/build-setup.md](docs/dev/build-setup.md).
- Development container: use the `container-*` presets; see
  [docs/dev/container-build.md](docs/dev/container-build.md). From the host, run
  commands through `./tools/dev-container`. Inside an IDE dev container, run
  CMake and test binaries directly.

Each preset builds into `build/<preset-name>/`, with binaries in
`build/<preset-name>/bin/`. The examples below use the native presets; the
container guide has the corresponding container commands.

For a full/clean rebuild redirect to a log to avoid cluttering your context.
Then always grep for warnings and only dump the full tail on failure.

```bash
cmake --build --preset gcc-debug > /tmp/build.log 2>&1; ec=$?
grep -n "warning:" /tmp/build.log
[ $ec -ne 0 ] && tail -100 /tmp/build.log
```

### Iterate (default): non-ASan, fastest edit-build-test

```bash
cmake --preset gcc-debug
cmake --build --preset gcc-debug
```

### Memory checks: ASan (run before committing, or when debugging a crash/UB)

```bash
cmake --preset gcc-debug-asan
cmake --build --preset gcc-debug-asan
```

Fresh checkout or after `rm -rf build/`: build once before IDE code-insight works (the
generated `*.pb.h` headers must exist). See [docs/dev/build-setup.md](docs/dev/build-setup.md).

## Test Commands

Use `--gtest_brief=1 --gtest_print_time=0` for any run where you don't need
to watch individual tests execute - only failing tests print output, passing
ones collapse to the final tally. This matters most for agents: gtest's
per-test/per-suite lines otherwise burn context on every green run.

```bash
# Run all tests (iteration build)
./build/gcc-debug/bin/luxir_test --gtest_brief=1 --gtest_print_time=0

# Run specific test suite
./build/gcc-debug/bin/luxir_test --gtest_filter="IndexWriterTest.*" --gtest_brief=1 --gtest_print_time=0

# Run benchmarks (NOTE: builds production-scale corpora - slow setup, use
# gcc-release and memory caps appropriate to the machine for real measurements.
# For quick iteration/coverage use the small-corpus unit-test mode instead:
#   ./build/gcc-debug/bin/luxir_test --gtest_filter="Benchmarks.all" --gtest_brief=1 --gtest_print_time=0
./build/gcc-release/bin/luxir_test --bench

# Run all benchmarks except the slow vector ones (HNSW/IVFPQ builds dominate
# wall-clock). Negative google-benchmark filter excludes the BM_Vector* family:
./build/gcc-release/bin/luxir_test --bench --benchmark_filter='-BM_Vector'

# Run the same suite under ASan before committing
./build/gcc-debug-asan/bin/luxir_test --gtest_brief=1 --gtest_print_time=0
```

## Code Conventions

- C-style casts for numeric values
- members at top of C++ classes, no prefix / suffix
- do not use em dashes or other non-ascii (in source or prose)
- This is unreleased code, so NEVER worry about back compat.
- Allocate engine objects that need a destructor (search ops, Query::Context,
  request/response wrappers) with luxir::arenaCreate<T> (src/luxir/util/proto.h),
  not protobuf's Arena::Create<T>. arenaCreate constructs first and registers the
  destructor only on success, so a throwing ctor is safe. Raw Arena::Create<T>
  registers the cleanup node before placement-new and runs ~T() on
  half-constructed memory if the ctor throws -> crash at arena reset.

## Writing Tests

- keep them short and maintainable
- never mock
- use test/test/CollectionHelper.h test/test/TestUtils.h and test/test/LocalReq.h

## Environment

- Locate dependency sources using the selected preset's vcpkg toolchain and
  installation paths; host and container paths can differ.
