#!/usr/bin/env bash
# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

# Fetch pinned source dependencies. Does not modify a vcpkg installation.
set -euo pipefail
cd "$(dirname "$0")"

FASTPFOR_REPO="${FASTPFOR_REPO:-https://github.com/fast-pack/FastPFOR.git}"
FASTPFOR_TAG=v0.5.0  # FastPFOR release with ARM NEON support

UNI_ALGO_TAG=v1.2.0  # Unicode 15.1.0; keep in sync with uni-algo/VENDORED.txt and CMakeLists.txt
UNI_ALGO_URL="${UNI_ALGO_URL:-https://github.com/uni-algo/uni-algo/archive/refs/tags/${UNI_ALGO_TAG}.tar.gz}"

# ---- FastPFOR: clone at the pinned tag if absent ----
if [ -d FastPFOR/.git ]; then
  echo "FastPFOR: source present (skipping fetch)"
else
  echo "FastPFOR: cloning ${FASTPFOR_REPO} @ ${FASTPFOR_TAG}"
  git clone --branch "${FASTPFOR_TAG}" --depth 1 "${FASTPFOR_REPO}" FastPFOR
fi

# ---- uni-algo: vendored in-repo; this only restores it if somehow absent ----
if [ -e uni-algo/VENDORED.txt ]; then
  echo "uni-algo: present (skipping fetch)"
else
  echo "uni-algo: fetching ${UNI_ALGO_URL}"
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  curl -fsSL "${UNI_ALGO_URL}" -o "$tmp/uni-algo.tar.gz"
  tar -xzf "$tmp/uni-algo.tar.gz" -C "$tmp"
  srcdir=("$tmp"/uni-algo-*)
  mkdir -p uni-algo
  cp -r "${srcdir[0]}/include" "${srcdir[0]}/src" "${srcdir[0]}/LICENSE.md" uni-algo/
  echo "${UNI_ALGO_TAG} (Unicode 15.1.0), vendored from https://github.com/uni-algo/uni-algo tag ${UNI_ALGO_TAG}" \
    > uni-algo/VENDORED.txt
  echo "Locally patched: patches/uni-algo-word-only-newline-leak.patch (word_only emitted newline runs after a word)" \
    >> uni-algo/VENDORED.txt
  git -C .. apply "$(pwd)/patches/uni-algo-word-only-newline-leak.patch"
fi
