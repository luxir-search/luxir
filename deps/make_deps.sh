#!/usr/bin/env bash
# Bootstrap deps/ for a fresh checkout:
#   1. fetch pinned third-party sources that are not checked into the repo
#      (simdcomp; also restores the normally-vendored uni-algo if absent)
#   2. apply Solux's local patches (delegates to apply_patches.sh, which
#      also patches the vcpkg roots - see its header for what and why)
#   3. build the simdcomp static libs if they are missing
# Safe to re-run: every step skips work that is already done.
#
# Usage: ./make_deps.sh [VCPKG_ROOT [VCPKG_ASAN_ROOT]]
#   (passed through to apply_patches.sh; defaults /opt/vcpkg /opt/vcpkg_asan)
#
# Env overrides, for mirrors / offline use:
#   SIMDCOMP_REPO  (default: https://github.com/lemire/SIMDCompressionAndIntersection.git)
#   UNI_ALGO_URL   (default: the github archive tarball for the pinned tag)
set -euo pipefail
cd "$(dirname "$0")"

SIMDCOMP_REPO="${SIMDCOMP_REPO:-https://github.com/lemire/SIMDCompressionAndIntersection.git}"
SIMDCOMP_COMMIT=b666a60c8fca18227d6532fb2d3b4d4dbc466cc9  # upstream master, 2020-12-11

UNI_ALGO_TAG=v1.2.0  # Unicode 15.1.0; keep in sync with uni-algo/VENDORED.txt and CMakeLists.txt
UNI_ALGO_URL="${UNI_ALGO_URL:-https://github.com/uni-algo/uni-algo/archive/refs/tags/${UNI_ALGO_TAG}.tar.gz}"

# ---- simdcomp: clone at the pinned commit if absent ----
if [ -d simdcomp/.git ]; then
  echo "simdcomp: source present (skipping fetch)"
else
  echo "simdcomp: cloning ${SIMDCOMP_REPO} @ ${SIMDCOMP_COMMIT}"
  git clone "${SIMDCOMP_REPO}" simdcomp
  git -C simdcomp checkout --quiet "${SIMDCOMP_COMMIT}"
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
  echo "Locally patched: patches/uni-algo-word-only-newline-leak.patch (word_only emitted newline runs after a word; see apply_patches.sh header)" \
    >> uni-algo/VENDORED.txt
  # apply_patches.sh (next step) re-applies the local uni-algo fix to this fresh copy
fi

# ---- local patches: vcpkg roots + the simdcomp checkout ----
./apply_patches.sh "$@"

# ---- simdcomp libs: build only if missing ----
# CMake links these from INSIDE the checkout (link_directories(deps/simdcomp)):
# libsimdcomp_a.a (release) for optimized builds, libsimdcomp_ad.a (DEBUG=1,
# minus _GLIBCXX_DEBUG via patches/simdcomp.diff) for debug builds.
# CXX/CC passed explicitly: the upstream Makefile defaults to g++-4.7.
if [ -f simdcomp/libsimdcomp_a.a ] && [ -f simdcomp/libsimdcomp_ad.a ]; then
  echo "simdcomp: libs present (skipping build)"
else
  echo "simdcomp: building libsimdcomp_ad.a (debug)"
  make -C simdcomp -s clean
  make -C simdcomp -s CXX=g++ CC=gcc DEBUG=1 -j"$(nproc)" libSIMDCompressionAndIntersection.a
  mv simdcomp/libSIMDCompressionAndIntersection.a simdcomp/libsimdcomp_ad.a
  echo "simdcomp: building libsimdcomp_a.a (release)"
  make -C simdcomp -s clean
  make -C simdcomp -s CXX=g++ CC=gcc -j"$(nproc)" libSIMDCompressionAndIntersection.a
  mv simdcomp/libSIMDCompressionAndIntersection.a simdcomp/libsimdcomp_a.a
  make -C simdcomp -s clean
fi

echo
echo "deps ready. Next: install packages into the vcpkg roots (see README.txt),"
echo "or, if they are already installed, rebuild any package whose port/triplet"
echo "just changed (e.g. faiss for FAISS_OPT_LEVEL=dd)."
