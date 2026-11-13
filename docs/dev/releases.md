# Building a release

The `v0.1` branch holds the 0.1 maintenance line. Annotated tags identify exact
releases: `v0.1.0`, then `v0.1.1` for a subsequent fix. Build all artifacts from
one clean commit on that branch. Keep the version in `CMakeLists.txt` in step
with the intended tag. Development continues on `main`.

Use the [development container](container-build.md). Its Ubuntu 22.04 baseline
keeps the minimum glibc version at 2.35. The three optimized executables target
x86-64-v2, v3, and v4; dependencies share the v2 baseline and retain their
runtime-selected kernels. Debug and ASan x86 downloads target v2. Native
ARM64 uses the same `container-release` preset and targets `armv8-a` on
Ubuntu 22.04. Run only the baseline presets on ARM64; v3/v4 are x86-only.

## Build and validate

Commit the release changes first. Executables embed the source commit, so a
later commit leaves every build stale. Run builds sequentially. Save
configure/build/test output to logs and inspect compiler warnings and any test
failures.

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

`tools/package-release` requires a clean checkout and an up-to-date build. It
packages the tested executable, checking its version, embedded source commit,
x86 required-ISA note, dynamic libraries, and glibc symbol requirements. It does not
replace the test and CPU validation above. Record the immutable builder image:

```bash
builder_image=$(docker image inspect "${LUXIR_DEV_IMAGE:-luxir-dev:jammy-gcc16}" \
  --format '{{.Id}}')
for preset in container-release container-release-v3 container-release-v4 \
              container-debug container-asan; do
  LUXIR_DEV_IMAGE="$builder_image" ./tools/dev-container tools/package-release \
    "build/$preset" --output build/releases/0.1.0 --builder "$(python3 -c 'import json,sys; print(json.dumps(dict(kind="container", image=sys.argv[1])))' "$builder_image")" || break
done
```

The result is three standalone stripped executables, five executable archives,
three matching symbol archives, and `SHA256SUMS`. The standalone executables
are named `luxir-0.1.0-linux-x86_64-v2`, `-v3`, and `-v4`, without an archive
suffix. They are byte-for-byte identical to the executables inside the
corresponding archives, and are included in the checksums. Downloading one
requires only `chmod +x` before running it; no unpacking is needed.

ARM64 packaging writes `luxir-<version>-linux-aarch64`, its `.tar.gz`,
and a matching `-symbols.tar.gz`, without a CPU tier suffix. Debug and ASan
packaging supports `-debug` and `-asan` suffixes. It checks the AArch64 loader
and the same glibc <= 2.35 and runtime library limits; ARM64 has no x86 ISA-note
check. On ARM64 run the packaging command for `container-release` only for
the nightly asset set. All packages written into the output directory are
covered by `SHA256SUMS`.

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

## Nightly binaries

The `Nightly binaries` Actions workflow runs daily at 06:37 UTC on the upstream
repository. It resolves `main` once and skips an unchanged `nightly` tag. To
force a rebuild, select **Actions > Nightly binaries > Run workflow**. Manual
runs also build `main`, including when dispatched from another branch; forks
can dispatch manually to publish to their own repository.

The rolling `nightly` prerelease contains the Linux x86-64 v2, v3, and v4
optimized executables, executable and symbol archives, v2 Debug and ASan
archives, the ARM64 release executable and its executable/symbol archives,
the macOS Apple silicon executable and its executable/symbol archives,
and one combined `SHA256SUMS`. Names retain the source version, so
use the release commit and packaged manifests to identify a particular nightly.
Both keyed dependency images (`<key>-amd64` and `<key>-arm64`) must already
have been published by `deps-image.yml`. The macOS dependency artifact must also exist. Missing any dependency fails planning.
These public images are pulled without registry sign-in.

Tests are advisory: each leg records all three test binaries' statuses and
output in its workflow artifact and job summary. Unsupported CPU tiers are
reported as `not run on this CPU`; containers use the hosted runner's CPU.
Release notes list the commit, builder images, and per-(platform, preset) test results.
Artifacts and full logs are retained for 14 days.

Every (platform, preset) leg must build and package successfully or nothing is published and
the previous nightly remains intact. Assets are uploaded to a staging draft
before replacing the previous release and moving the `nightly` tag. GitHub
does not offer an atomic release/tag replacement: a failure during the final
replacement can require manual recovery from the staging draft. A failed
upload leaves the previous nightly untouched and may leave a staging draft.
Tag rules must allow the workflow token to move `nightly`; release immutability
must not be enabled for this rolling prerelease.

For local execution of one leg from a clean checkout with initialized
submodules, set `LUXIR_DEV_IMAGE` to the keyed registry image and run
`BUILD_JOBS=$(nproc) tools/nightly-build container-release`. Use a fresh
`build/nightly/linux-amd64-container-release` (or `linux-arm64-container-release`) output directory for each run. Packaging
checks embedded metadata without execution on CPUs that cannot run the selected
tier; the manifest records whether the runtime version check was performed.

## macOS nightly

The `macos-arm64/macos-release` leg builds natively on `macos-15` (Apple M1).
Assets are `luxir-<version>-macos-arm64`, `.tar.gz`, and `-symbols.tar.gz`.
The baseline is macOS 15.0 and apple-m1. GCC runtimes are static; Apple system
libraries and Accelerate remain dynamic. Packaging checks the thin arm64
architecture, exact minimum OS, and an explicit system-library allowlist.
It generates `luxir.dSYM` with dsymutil before stripping, checks matching UUIDs,
then strips, signs ad hoc and verifies the final executable. LLDB can load the
matching dSYM beside the executable.

Downloads are not notarized or signed by a developer identity. For a browser
download, verify SHA256SUMS, then remove quarantine and enable execution:

```bash
xattr -d com.apple.quarantine ./luxir-<version>-macos-arm64
chmod +x ./luxir-<version>-macos-arm64
```

`deps-image.yml` has an independent, main-only macOS job. It publishes an OCI
artifact in the existing public `luxir-dev` package using ORAS. No new package
or Actions cache is used. The tag is `<key>-macos-arm64`; the installed tree
and pinned vcpkg checkout restore to `/opt/vcpkg`, and the recorded recipes
to `/opt/luxir-deps`. Restoration requires both paths to be absent, to avoid
mixing installations. It downloads anonymously by immutable manifest digest.
`tools/macos-toolchain setup` installs hosted tools. `tools/macos-deps publish`
builds and publishes; `tools/macos-deps restore` restores for a build. For local
use set `GITHUB_REPOSITORY_OWNER`; publication additionally requires REGISTRY,
GITHUB_REPOSITORY, GITHUB_ACTOR and GH_TOKEN.

The shared dependency-key tool retains the Linux Dockerfile contract and adds
a native input set. The native key covers manifests, overlays, triplets and
included baseline, setup/build scripts, full GCC C/C++ versions, macOS version,
Xcode and SDK. Hosted toolchain changes require republishing dependencies.
Planning runs on macOS so it can resolve that identity, and uses ORAS for
both image and native artifact existence checks without Docker. Each native
builder record includes toolchain identity and dependency manifest digest.
Tests remain advisory and run without ASAN_OPTIONS on macOS.

Changing macOS triplets changes the Linux image key too: the Dockerfile copies
all triplets and the key hashes all of them. This change therefore requires
republishing both Linux images as well as the macOS artifact.
