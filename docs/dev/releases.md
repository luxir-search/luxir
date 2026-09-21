# Building a release

The `v0.1` branch holds the 0.1 maintenance line. Annotated tags identify exact
releases: `v0.1.0`, then `v0.1.1` for a subsequent fix. Build all artifacts from
one clean commit on that branch. Keep the version in `CMakeLists.txt` in step
with the intended tag. Development continues on `main`.

Use the [development container](container-build.md). Its Ubuntu 22.04 baseline
keeps the minimum glibc version at 2.35. The three optimized executables target
x86-64-v2, v3, and v4; dependencies share the v2 baseline and retain their
runtime-selected kernels. Debug and ASan downloads target v2.

## Build and validate

Run builds sequentially. Save configure/build/test output to logs and inspect
compiler warnings and any test failures.

```bash
for preset in container-release container-release-v3 container-release-v4 \
              container-debug container-asan; do
  ./tools/dev-container cmake --preset "$preset" &&
  ./tools/dev-container cmake --build --preset "$preset" &&
  ./tools/dev-container env ASAN_OPTIONS=detect_leaks=1:allow_addr2line=1 \
    "build/$preset/bin/luxir_test" --gtest_brief=1 --gtest_print_time=0 || break
done
```

Also run `decoded_successor_no_avx512_test` and `decoded_successor_scalar_test`
from each build's `bin/` directory. Internally, configure ASan at v3 and v4 to
cover engine code absent from the v2 diagnostic build:

```bash
./tools/dev-container cmake --preset container-asan -B build/container-asan-v3 \
  -DLUXIR_CPU_TARGET=x86-64-v3
./tools/dev-container cmake --build build/container-asan-v3
./tools/dev-container env ASAN_OPTIONS=detect_leaks=1:allow_addr2line=1 \
  build/container-asan-v3/bin/luxir_test --gtest_brief=1 --gtest_print_time=0
```

Repeat with v4. Validate final binaries on the minimum OS and restricted CPU
levels, including dispatched vector kernels, HTTP/gRPC, persistent indexes
reopened across tiers, and rejection of unsupported CPUs. Containers use the
host CPU; they alone do not establish ISA compatibility. For instruction
checking, Intel SDE's `-nhm`, `-hsw`, and `-skx` models cover the three baseline
families. First verify the checker rejects deliberate forbidden instructions.

## Package

Commit the release changes after validation. `tools/package-release` requires a
clean checkout and an up-to-date build. It packages the tested executable, checking its version,
required-ISA note, dynamic libraries, and glibc symbol requirements. It does not
replace the test and CPU validation above. Record the immutable builder image:

```bash
builder_image=$(docker image inspect "${LUXIR_DEV_IMAGE:-luxir-dev:jammy-gcc16}" \
  --format '{{.Id}}')
for preset in container-release container-release-v3 container-release-v4 \
              container-debug container-asan; do
  LUXIR_DEV_IMAGE="$builder_image" ./tools/dev-container tools/package-release \
    "build/$preset" --output build/releases/0.1.0 --builder-image "$builder_image" || break
done
```

The result is three standalone stripped executables, five executable archives,
three matching symbol archives, and `SHA256SUMS`. The standalone executables
are named `luxir-0.1.0-linux-x86_64-v2`, `-v3`, and `-v4`, without an archive
suffix. They are byte-for-byte identical to the executables inside the
corresponding archives, and are included in the checksums. Downloading one
requires only `chmod +x` before running it; no unpacking is needed.

Each executable package contains its build manifest, installed
dependency inventory, license notices, API definitions, and guides. The
manifest records the source revision, CPU target, flags, compiler, dependency
pins, builder image, executable checksum, and ELF build ID.

Release symbols come from the exact optimized executable using `objcopy`.
Extract the matching `-symbols` archive beside the executable package; it places
`luxir.debug` next to `luxir`, where GDB finds it through `.gnu_debuglink`.
Verify that GDB resolves `main` to a source line with the split file. Source
paths use `/workspace/luxir`; use GDB's `set substitute-path` for a local checkout.
Optimized dependency archives retain available function symbols but lack full
source-level debugging information. Debug/ASan downloads include their
dependency debug information.

Verify extracted archives, standalone executables, and checksums, then tag that same commit:

```bash
git tag -a v0.1.0 -m 'Luxir 0.1.0'
```

Keep published tags fixed. A subsequent fix gets a new patch version and tag.
Uploading artifacts and pushing the branch/tag are separate publication steps.
