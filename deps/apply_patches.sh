#!/usr/bin/env bash
# Apply Solux's local patches to the vcpkg roots and the FastPFOR checkout.
# Safe to re-run: patches that are already applied are detected and skipped.
#
# Usage: ./apply_patches.sh [VCPKG_ROOT [VCPKG_ASAN_ROOT]]
#   defaults: /opt/vcpkg  /opt/vcpkg_asan
#
# What gets patched, and why:
#
#   <vcpkg>/triplets/x64-linux.cmake (both roots)
#     -mavx -mssse3 -march=native -mtune=native, matching the project's own
#     release CXXFLAGS. Required for correctness, not just speed: abseil's
#     hash changes with these flags, and mixing flag-sets makes
#     protobuf::Map<string,...>::find() silently return end() for entries
#     that iteration sees (release builds only). The asan-root triplet
#     additionally builds every dependency with -fsanitize=address; without
#     that, newer gRPC/protobuf hit "use after poison" under the asan presets.
#
#   <vcpkg>/ports/faiss/portfile.cmake (both roots)
#     FAISS_OPT_LEVEL=dd (runtime SIMD dispatch). Without an opt level, the
#     handwritten AVX2/AVX512 kernel variants (distances, scalar quantizer,
#     PQ code distance) are never compiled and FAISS distance kernels run
#     SSE-only. Takes effect on the next faiss build (see reminder below).
#
#   <vcpkg>/ports/openblas/portfile.cmake (both roots; ASan conditional)
#     OpenBLAS selects an AVX512 Cooper Lake kernel on Zen 5. GCC cannot add
#     ASan instrumentation to its register-saturated inline assembly, so ASan
#     builds use the portable AVX2 Haswell kernel instead. Normal builds keep
#     native CPU detection.
#
#   deps/FastPFOR (cloned by make_deps.sh; gitignored)
#     patches/fastpfor*.diff (if any): local fixes to the vendored FastPFOR.
#     None are needed at present; the hook below applies them if/when added.
#
#   deps/uni-algo (vendored in-repo, normally already patched in git)
#     patches/uni-algo-word-only-newline-leak.patch: upstream v1.2.0 bug -
#     the per-segment word property accumulator is reset only on a WB999
#     break, not on WB3a/WB3b newline breaks, so a CR/LF/Newline run right
#     after a word inherits its property and word_only emits it as a "word"
#     ("sep\r\nlines" -> "sep", "\r\n", "lines"; newline runs became index
#     terms). The fix resets the accumulator in WB3a/WB3b exactly like WB999
#     (all four variants: utf8/utf16 x forward/reverse). Word BOUNDARIES are
#     untouched - UAX#29 conformance is unaffected (gated in AnalysisTest,
#     which also equivalence-checks word_only against the ASCII fast path).
#     This entry only matters if uni-algo is ever re-fetched from upstream.
set -euo pipefail
cd "$(dirname "$0")"
PATCH_DIR="$(pwd)/patches"  # absolute: git -C <repo> resolves relative paths in <repo>

VCPKG_ROOT="${1:-/opt/vcpkg}"
VCPKG_ASAN_ROOT="${2:-/opt/vcpkg_asan}"

apply() {  # apply <repo-dir> <patch-file>
  local repo=$1 patch=$2
  local name
  name=$(basename "$patch")
  if [ ! -d "$repo" ]; then
    echo "  skip    $name ($repo does not exist)"
    return 0
  fi
  if git -C "$repo" apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "  ok      $name (already applied)"
  elif git -C "$repo" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$repo" apply "$patch"
    echo "  APPLIED $name"
  else
    echo "  FAILED  $name does not apply cleanly to $repo" >&2
    echo "          Upstream probably changed; hand-merge using the patch as intent," >&2
    echo "          then regenerate it from 'git -C $repo diff'." >&2
    return 1
  fi
}

echo "vcpkg: $VCPKG_ROOT"
apply "$VCPKG_ROOT" "$PATCH_DIR"/vcpkg-triplet-x64-linux.patch
apply "$VCPKG_ROOT" "$PATCH_DIR"/vcpkg-faiss-opt-level-dd.patch
apply "$VCPKG_ROOT" "$PATCH_DIR"/vcpkg-openblas-asan-avx2.patch

echo "vcpkg (asan): $VCPKG_ASAN_ROOT"
apply "$VCPKG_ASAN_ROOT" "$PATCH_DIR"/vcpkg-asan-triplet-x64-linux.patch
apply "$VCPKG_ASAN_ROOT" "$PATCH_DIR"/vcpkg-faiss-opt-level-dd.patch
apply "$VCPKG_ASAN_ROOT" "$PATCH_DIR"/vcpkg-openblas-asan-avx2.patch

echo "uni-algo (vendored): $(pwd)/uni-algo"
apply .. "$PATCH_DIR"/uni-algo-word-only-newline-leak.patch

echo "FastPFOR: $(pwd)/FastPFOR"
if [ -d FastPFOR/.git ]; then
  shopt -s nullglob
  fastpfor_patches=("$PATCH_DIR"/fastpfor*.diff "$PATCH_DIR"/fastpfor*.patch)
  shopt -u nullglob
  if [ ${#fastpfor_patches[@]} -eq 0 ]; then
    echo "  ok      no FastPFOR patches yet"
  else
    for p in "${fastpfor_patches[@]}"; do apply FastPFOR "$p"; done
  fi
else
  echo "  skip    not cloned; run make_deps.sh first"
fi

echo
echo "Note: this script only patches; it builds nothing. Triplet/port changes"
echo "take effect when a package is (re)built, e.g.:"
echo "  cd $VCPKG_ROOT && ./vcpkg remove faiss && ./vcpkg install faiss"
echo "Rebuild faiss in $VCPKG_ROOT before benchmarking vector search, so the"
echo "AVX2 kernels (FAISS_OPT_LEVEL=dd) are actually in the library."
