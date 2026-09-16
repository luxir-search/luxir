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

# Track the effective CPU options in vcpkg's binary cache, including tuning and
# features masked by a VM. Keep the compiler selection consistent with this probe.
export CC=${CC:-gcc} CXX=${CXX:-g++}
native_cpu=$({
  LC_ALL=C "$CC" -march=native -mtune=native -Q --help=target --help=params
  LC_ALL=C "$CXX" -march=native -mtune=native -Q --help=target --help=params
} | sha256sum)
export LUXIR_NATIVE_CPU=${native_cpu%% *}

# Manifest reconciliation owns each prefix. Keep normal, ASan and legacy
# classic-mode installs separate. Host code generators always run unsanitized.
# ASan's separate host packages need only Release; normal builds reuse their
# target packages for host tools.
export VCPKG_MAX_CONCURRENCY=${VCPKG_MAX_CONCURRENCY:-12}
for variant in native native-asan; do
  host_triplet=x64-linux-luxir-native
  if [[ $variant == native-asan ]]; then
    host_triplet=x64-linux-luxir-native-host
  fi
  "$vcpkg_root/vcpkg" install \
    --x-manifest-root="$deps_dir" \
    --x-install-root="$vcpkg_root/installed-$variant" \
    --triplet="x64-linux-luxir-$variant" \
    --host-triplet="$host_triplet" \
    --clean-buildtrees-after-build --clean-packages-after-build
done
