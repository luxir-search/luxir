// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "BranchlessSearch.h"
#include "luxir_util.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>

#if defined(__AVX512F__) || defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(__AVX512F__) || defined(__AVX2__)
#define LUXIR_DECODED_SUCCESSOR_SIMD 1
#else
#define LUXIR_DECODED_SUCCESSOR_SIMD 0
#endif

#ifndef LUXIR_PROBE_CONSTANT_HOOKS
#define LUXIR_PROBE_CONSTANT_HOOKS 1
#endif

#ifndef LUXIR_FULL_INLINE_DECODED_SUCCESSOR
#define LUXIR_FULL_INLINE_DECODED_SUCCESSOR 0
#endif

#ifndef LUXIR_DECODED_SUCCESSOR_HISTOGRAM
#define LUXIR_DECODED_SUCCESSOR_HISTOGRAM 0
#endif

namespace luxir {

// Return the first index in [start, end) whose value is >= target. The input
// must be sorted, values before start must be below target, and a result must
// exist. No storage beyond end is part of the contract.
class DecodedSuccessor {
  static constexpr int32_t SCALAR_LINEAR_PROBE = 8;

#if defined(__AVX512F__)
  using Vec = __m512i;
  static constexpr int32_t LANES = 16;

  static Vec broadcastTarget(int32_t target) LUXIR_INLINE {
    return _mm512_set1_epi32(target);
  }

  static uint32_t geqMask(const int32_t* values, Vec target) LUXIR_INLINE {
    const Vec block = _mm512_loadu_si512((const void*) values);
    return (uint32_t) _mm512_cmp_epi32_mask(block, target, _MM_CMPINT_GE);
  }

  static uint32_t shortGeqMask(const int32_t* values, int32_t length,
                               Vec target) LUXIR_INLINE {
    assert(length > 0 && length < LANES);
    const uint32_t valid = (1u << length) - 1u;
    const Vec block =
        _mm512_maskz_loadu_epi32((__mmask16) valid, (const void*) values);
    return (uint32_t) _mm512_cmp_epi32_mask(
        block, target, _MM_CMPINT_GE) & valid;
  }

#elif defined(__AVX2__)
  using Vec = __m256i;
  static constexpr int32_t LANES = 8;

  static Vec broadcastTarget(int32_t target) LUXIR_INLINE {
    // Signed block > target - 1 implements block >= target.
    assert(target != INT32_MIN);
    return _mm256_set1_epi32(target - 1);
  }

  static uint32_t geqMask(const int32_t* values, Vec target) LUXIR_INLINE {
    const Vec block =
        _mm256_loadu_si256((const __m256i*) values);
    const Vec cmp = _mm256_cmpgt_epi32(block, target);
    return (uint32_t) _mm256_movemask_ps(_mm256_castsi256_ps(cmp));
  }

  static uint32_t shortGeqMask(const int32_t* values, int32_t length,
                               Vec target) LUXIR_INLINE {
    assert(length > 0 && length < LANES);
    const uint32_t valid = (1u << length) - 1u;
    const Vec laneMask = _mm256_cmpgt_epi32(
        _mm256_set1_epi32(length),
        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
    const Vec block = _mm256_maskload_epi32(values, laneMask);
    const Vec cmp = _mm256_cmpgt_epi32(block, target);
    return (uint32_t) _mm256_movemask_ps(
        _mm256_castsi256_ps(cmp)) & valid;
  }

#endif

#if LUXIR_DECODED_SUCCESSOR_SIMD
  static constexpr int32_t HISTOGRAM_BUCKETS = 128 / LANES + 2;
#else
  static constexpr int32_t HISTOGRAM_BUCKETS = 10;
#endif

  static void recordStrides(int32_t strides) LUXIR_INLINE {
#if LUXIR_DECODED_SUCCESSOR_HISTOGRAM
    assert(strides >= 0 && strides < HISTOGRAM_BUCKETS);
    vectorStridesForTests[(size_t) strides]++;
#else
    (void) strides;
#endif
  }

  static int32_t scalarIndex(const int32_t* values, int32_t start,
                             int32_t end, int32_t target) LUXIR_INLINE {
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

#if LUXIR_DECODED_SUCCESSOR_SIMD
  static int32_t LUXIR_NOINLINE shortArrayIndex(
      const int32_t* values, int32_t start, int32_t end, Vec target,
      int32_t strides) {
    const uint32_t mask = shortGeqMask(values + start, end - start, target);
    strides++;
    assert(mask != 0);
    recordStrides(strides);
    return start + (int32_t) std::countr_zero(mask);
  }

  static int32_t LUXIR_NOINLINE continuationIndex(
      const int32_t* values, int32_t start, int32_t end, Vec target,
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
      if (end < LANES) {
        return shortArrayIndex(values, j, end, target, strides);
      }
      // The final vector ends at end and may overlap values before j. Those
      // lanes are below target by contract, while a matching lane is known to
      // exist in [j, end), so no lane mask or not-found branch is needed.
      const int32_t base = end - LANES;
      const uint32_t mask = geqMask(values + base, target);
      strides++;
      assert(mask != 0);
      recordStrides(strides);
      return base + (int32_t) std::countr_zero(mask);
    }
    recordStrides(strides);
    return end;
  }
#endif

public:
#if LUXIR_DECODED_SUCCESSOR_HISTOGRAM
  static inline std::array<uint64_t, HISTOGRAM_BUCKETS>
      vectorStridesForTests{};
#endif

#if LUXIR_PROBE_CONSTANT_HOOKS
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

  static constexpr bool hasAvx2ForTests() {
#if defined(__AVX2__) && !defined(__AVX512F__)
    return true;
#else
    return false;
#endif
  }

  static int32_t index(const int32_t* values, int32_t start, int32_t end,
                       int32_t target) LUXIR_INLINE {
    assert(values != nullptr);
    assert(start >= 0 && start < end);
    assert(start == 0 || values[start - 1] < target);
    assert(values[end - 1] >= target);
#if LUXIR_DECODED_SUCCESSOR_SIMD
    if (disableSimdForTests) {
      return scalarIndex(values, start, end, target);
    }

    const Vec targetVector = broadcastTarget(target);
#if LUXIR_FULL_INLINE_DECODED_SUCCESSOR
    if (end < LANES) {
      return scalarIndex(values, start, end, target);
    }
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
      const int32_t base = end - LANES;
      const uint32_t mask = geqMask(values + base, targetVector);
      strides++;
      assert(mask != 0);
      recordStrides(strides);
      return base + (int32_t) std::countr_zero(mask);
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

} // namespace luxir

#undef LUXIR_DECODED_SUCCESSOR_SIMD
