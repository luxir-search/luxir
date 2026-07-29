#pragma once

#include "BranchlessSearch.h"
#include "solux_util.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>

#if defined(__AVX512F__)
#include <immintrin.h>
#endif

#ifndef SOLUX_PROBE_CONSTANT_HOOKS
#define SOLUX_PROBE_CONSTANT_HOOKS 1
#endif

#ifndef SOLUX_FULL_INLINE_DECODED_SUCCESSOR
#define SOLUX_FULL_INLINE_DECODED_SUCCESSOR 0
#endif

#ifndef SOLUX_DECODED_SUCCESSOR_HISTOGRAM
#define SOLUX_DECODED_SUCCESSOR_HISTOGRAM 0
#endif

namespace solux {

// Return the first index in [start, end) whose value is >= target, or end.
// The input must be sorted. No storage beyond end is part of the contract.
class DecodedSuccessor {
  static constexpr int32_t LANES = 16;
  static constexpr int32_t SCALAR_LINEAR_PROBE = 8;
  static constexpr int32_t HISTOGRAM_BUCKETS = 10;

  static void recordStrides(int32_t strides) SOLUX_INLINE {
#if SOLUX_DECODED_SUCCESSOR_HISTOGRAM
    assert(strides >= 0 && strides < HISTOGRAM_BUCKETS);
    vectorStridesForTests[(size_t) strides]++;
#else
    (void) strides;
#endif
  }

  static int32_t scalarIndex(const int32_t* values, int32_t start,
                             int32_t end, int32_t target) SOLUX_INLINE {
    int32_t j = start;
    const int32_t linearEnd =
        std::min(end, start + SCALAR_LINEAR_PROBE);
    while (j < linearEnd && values[j] < target) {
      j++;
    }
    if (j < linearEnd) {
      return j;
    }
    return (int32_t) (BranchlessIndex<int32_t>::lowerBound(
        values + j, (size_t) (end - j), target) - values);
  }

#if defined(__AVX512F__)
  static uint32_t geqMask(const int32_t* values, __m512i target) SOLUX_INLINE {
    const __m512i block = _mm512_loadu_si512((const void*) values);
    return (uint32_t) _mm512_cmp_epi32_mask(block, target, _MM_CMPINT_GE);
  }

  static uint32_t tailGeqMask(const int32_t* values, int32_t length,
                              __m512i target) SOLUX_INLINE {
    assert(length > 0 && length < LANES);
    const uint32_t valid = (1u << length) - 1u;
    const __m512i block =
        _mm512_maskz_loadu_epi32((__mmask16) valid, (const void*) values);
    return (uint32_t) _mm512_cmp_epi32_mask(
        block, target, _MM_CMPINT_GE) & valid;
  }

  static int32_t SOLUX_NOINLINE continuationIndex(
      const int32_t* values, int32_t start, int32_t end, __m512i target,
      int32_t strides) {
    int32_t j = start;
    while (end - j >= LANES) {
      const uint32_t mask = geqMask(values + j, target);
      strides++;
      if (mask != 0) {
        recordStrides(strides);
        return j + (int32_t) std::countr_zero(mask);
      }
      j += LANES;
    }
    if (j < end) {
      const uint32_t mask = tailGeqMask(values + j, end - j, target);
      strides++;
      if (mask != 0) {
        recordStrides(strides);
        return j + (int32_t) std::countr_zero(mask);
      }
    }
    recordStrides(strides);
    return end;
  }
#endif

public:
#if SOLUX_DECODED_SUCCESSOR_HISTOGRAM
  static inline std::array<uint64_t, HISTOGRAM_BUCKETS>
      vectorStridesForTests{};
#endif

#if SOLUX_PROBE_CONSTANT_HOOKS
  static inline bool disableSimdForTests = false;
#else
  static constexpr bool disableSimdForTests = false;
#endif

  static constexpr bool hasAvx512ForTests() {
#if defined(__AVX512F__)
    return true;
#else
    return false;
#endif
  }

  static int32_t index(const int32_t* values, int32_t start, int32_t end,
                       int32_t target) SOLUX_INLINE {
    assert(values != nullptr);
    assert(start >= 0 && start <= end);
#if defined(__AVX512F__)
    if (disableSimdForTests) {
      return scalarIndex(values, start, end, target);
    }

    const __m512i targetVector = _mm512_set1_epi32(target);
#if SOLUX_FULL_INLINE_DECODED_SUCCESSOR
    int32_t j = start;
    int32_t strides = 0;
    while (end - j >= LANES) {
      const uint32_t mask = geqMask(values + j, targetVector);
      strides++;
      if (mask != 0) {
        recordStrides(strides);
        return j + (int32_t) std::countr_zero(mask);
      }
      j += LANES;
    }
    if (j < end) {
      const uint32_t mask =
          tailGeqMask(values + j, end - j, targetVector);
      strides++;
      if (mask != 0) {
        recordStrides(strides);
        return j + (int32_t) std::countr_zero(mask);
      }
    }
    recordStrides(strides);
    return end;
#else
    if (end - start >= LANES) {
      const uint32_t mask = geqMask(values + start, targetVector);
      if (mask != 0) {
        recordStrides(1);
        return start + (int32_t) std::countr_zero(mask);
      }
      start += LANES;
      return continuationIndex(values, start, end, targetVector, 1);
    }
    return continuationIndex(values, start, end, targetVector, 0);
#endif
#else
    return scalarIndex(values, start, end, target);
#endif
  }
};

} // namespace solux
