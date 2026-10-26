#!/usr/bin/env bash
# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

# Native setup: build the shared dependency manifest for the host CPU.
# For the container build, use fetch_sources.sh by itself.
# Usage: ./make_deps.sh [VCPKG_ROOT]
set -euo pipefail
if (( $# > 1 )); then
  echo "Usage: $0 [VCPKG_ROOT]" >&2
  exit 1
fi
vcpkg_root=$(realpath "${1:-/opt/vcpkg}")
cd "$(dirname "$0")"
deps_dir=$PWD
revision=$(python3 -c 'import json; print(json.load(open("vcpkg.json"))["builtin-baseline"])')
if [[ $(git -C "$vcpkg_root" rev-parse HEAD) != "$revision" ]]; then
  echo "Check out the builtin-baseline from deps/vcpkg.json in $vcpkg_root and bootstrap vcpkg first." >&2
  exit 1
fi
./fetch_sources.sh

export CC=${CC:-gcc-16} CXX=${CXX:-g++-16}
if [[ $(uname -s) == Darwin ]]; then
  # A fixed apple-m1 baseline, so nothing CPU-specific to fingerprint.
  # jemalloc is not yet supported on macOS.
  triplet=arm64-osx-luxir-m1
  jemalloc=()
else
  # Track the effective CPU options in vcpkg's binary cache, including tuning and
  # features masked by a VM. Keep the compiler selection consistent with this probe.
  native_cpu=$({
    LC_ALL=C "$CC" -march=native -mtune=native -Q --help=target --help=params
    LC_ALL=C "$CXX" -march=native -mtune=native -Q --help=target --help=params
  } | sha256sum)
  export LUXIR_NATIVE_CPU=${native_cpu%% *}
  triplet=x64-linux-luxir-native
  jemalloc=(--x-feature=jemalloc)
fi

# Optional extra manifest features (comma-separated, e.g. "icu") for the normal
# native install, from LUXIR_VCPKG_FEATURES.
IFS=, read -ra extra <<< "${LUXIR_VCPKG_FEATURES:-}"
for f in ${extra[@]+"${extra[@]}"}; do
  [[ -z $f ]] || jemalloc+=("--x-feature=$f")
done

# Manifest reconciliation owns each prefix. Keep normal, ASan and legacy
# classic-mode installs separate. Host code generators always run unsanitized.
# ASan's separate host packages need only Release; normal builds reuse their
# target packages for host tools. Only normal builds link jemalloc; ASan
# supplies its own allocator.
# Dependency build jobs default to what the cores and memory allow
# (tools/build-jobs); ASan compiles need more memory each.
jobs=${VCPKG_MAX_CONCURRENCY:-}
for variant in native native-asan; do
  target_triplet=$triplet
  host_triplet=$triplet
  features=(${jemalloc[@]+"${jemalloc[@]}"})
  job_mb=2500
  if [[ $variant == native-asan ]]; then
    target_triplet=$triplet-asan
    host_triplet=$triplet-host
    features=()
    job_mb=3500
  fi
  # Guarded expansions keep empty arrays valid under set -u in bash 3.2.
  VCPKG_MAX_CONCURRENCY=${jobs:-$(../tools/build-jobs "$job_mb")} "$vcpkg_root/vcpkg" install \
    --x-manifest-root="$deps_dir" \
    --x-install-root="$vcpkg_root/installed-$variant" \
    --triplet="$target_triplet" \
    --host-triplet="$host_triplet" \
    ${features[@]+"${features[@]}"} \
    --clean-buildtrees-after-build --clean-packages-after-build
done
