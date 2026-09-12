#!/usr/bin/env bash
# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

# Native setup: fetch pinned sources and patch the two native vcpkg roots.
# For the container build, use fetch_sources.sh by itself.
# Usage: ./make_deps.sh [VCPKG_ROOT [VCPKG_ASAN_ROOT]]
set -euo pipefail
cd "$(dirname "$0")"
./fetch_sources.sh
./apply_patches.sh "$@"
