#!/usr/bin/env bash
# Bootstrap deps/ for a fresh checkout:
#   1. fetch pinned third-party sources that are not checked into the repo
#      (FastPFOR; also restores the normally-vendored uni-algo if absent)
#   2. apply Luxir's local patches (delegates to apply_patches.sh, which
#      also patches the vcpkg roots - see its header for what and why)
# Safe to re-run: every step skips work that is already done.
#
# FastPFOR is compiled by the main CMakeLists.txt (the fastpfor target builds a
# subset of its src/), so unlike the old simdcomp setup there is NO separate
# static-lib build step here - the cloned source + headers just need to exist.
#
# Usage: ./make_deps.sh [VCPKG_ROOT [VCPKG_ASAN_ROOT]]
#   (passed through to apply_patches.sh; defaults /opt/vcpkg /opt/vcpkg_asan)
#
# Env overrides, for mirrors / offline use:
#   FASTPFOR_REPO  (default: https://github.com/fast-pack/FastPFOR.git)
#   UNI_ALGO_URL   (default: the github archive tarball for the pinned tag)
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
  echo "Locally patched: patches/uni-algo-word-only-newline-leak.patch (word_only emitted newline runs after a word; see apply_patches.sh header)" \
    >> uni-algo/VENDORED.txt
  # apply_patches.sh (next step) re-applies the local uni-algo fix to this fresh copy
fi

# ---- local patches: vcpkg roots, uni-algo, and any FastPFOR patches ----
./apply_patches.sh "$@"

echo
echo "deps ready. Next: install packages into the vcpkg roots (see README.txt),"
echo "or, if they are already installed, rebuild any package whose port/triplet"
echo "just changed (e.g. faiss for FAISS_OPT_LEVEL=dd)."
