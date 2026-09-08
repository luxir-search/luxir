// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <bit>
#include <type_traits>

namespace luxir {

/// Branchless (monobound) binary search over a sorted, contiguous array.
///
/// Each probe is a `cmov`/`csel`-friendly conditional pointer move, so the loop
/// runs a fixed log2(n) iterations with no data-dependent branches.  This wins
/// over std::lower_bound when (a) the array is cache-resident and (b) the search
/// keys are unpredictable, so a branchy search would mispredict.  The flip side
/// is a load->cmov->load dependency chain with no speculation and no prefetch:
/// once the data spills past L2 the chain latency dominates and a branchy /
/// prefetching search wins.  Benchmark against the real working set before
/// committing to it.
///
/// The stateless statics are the primary entry point: layout constants are a
/// single `bit_floor` + subtract, so there is nothing worth precomputing for
/// arrays whose length varies per call.  The stateful ctor only earns its keep
/// when one fixed-length array is searched repeatedly.
///
/// The element type T must be cheap to copy and compare; a heavy operator< would
/// dominate the (otherwise tiny) probe cost.  The key type is deduced per call
/// and may differ from T (e.g. a uint16_t array searched with an int32_t key),
/// as long as `elem < key` and `key < elem` are well defined.
template <typename T>
class BranchlessIndex {
  static_assert(std::is_trivially_copyable_v<T>,
    "Branchless search assumes a cheap, trivially copyable element with a side-effect-free operator<.");

  // Layout for a fixed-length array: step is the largest power of two <= len,
  // remainder = len - step (always < step), the slice folded away by the first
  // probe so the main loop runs over a clean power-of-two window.
  size_t step{0};
  size_t remainder{0};

  static void layout(size_t len, size_t& step, size_t& remainder) noexcept {
    if (len <= 1) { step = len; remainder = 0; return; }
    step = std::bit_floor(len);
    remainder = len - step;
  }

  // "Should we move past the element at base[off]?"  Inclusive==false gives
  // lower_bound (advance while elem < key -> land on first >= key); Inclusive==true
  // gives upper_bound (advance while elem <= key -> land on first > key).  Phrased
  // via operator< only, like the STL.
  template <bool Inclusive, typename K>
  static bool advancePast(const T& a, const K& key) noexcept {
    if constexpr (Inclusive) return !(key < a); // a <= key
    else return a < key;
  }

  template <bool Inclusive, typename K>
  static const T* core(const T* begin, size_t step, size_t remainder, const K& key) noexcept {
    if (step == 0) [[unlikely]] return begin; // empty array
    const T* base = begin;
    if (remainder > 0)
      base = advancePast<Inclusive>(base[remainder], key) ? base + remainder : base;
    for (size_t s = step; s > 1; ) {
      s >>= 1;
      base = advancePast<Inclusive>(base[s], key) ? base + s : base;
    }
    return base + advancePast<Inclusive>(*base, key);
  }

public:
  BranchlessIndex() = default;
  explicit BranchlessIndex(size_t len) noexcept { layout(len, step, remainder); }

  /// First element not less than key (i.e. first >= key), or begin+len if none.
  template <typename K>
  [[nodiscard]] static const T* lowerBound(const T* begin, size_t len, const K& key) noexcept {
    size_t step, remainder; layout(len, step, remainder);
    return core<false>(begin, step, remainder, key);
  }

  /// First element greater than key (i.e. first > key), or begin+len if none.
  /// For "index of the slot that owns key k" over a strictly-increasing prefix
  /// array, use (upperBound(begin, len, k) - begin - 1).
  template <typename K>
  [[nodiscard]] static const T* upperBound(const T* begin, size_t len, const K& key) noexcept {
    size_t step, remainder; layout(len, step, remainder);
    return core<true>(begin, step, remainder, key);
  }

  /// Stateful variants: layout is precomputed for one fixed length (see ctor).
  template <typename K>
  [[nodiscard]] const T* lowerBound(const T* begin, const K& key) const noexcept {
    return core<false>(begin, step, remainder, key);
  }
  template <typename K>
  [[nodiscard]] const T* upperBound(const T* begin, const K& key) const noexcept {
    return core<true>(begin, step, remainder, key);
  }
};

} // namespace luxir
