// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "luxir/util/luxir_util.h"

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__) || defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/// Population counts over packed bits, shared by the bit-vector scorers: the
/// hamming metric over the bit encoding (popcount(a ^ b)) and RaBitQScorer's
/// bit-plane dot products (popcount(code & plane)).
///
/// The instruction set follows the build's CPU target (LUXIR_CPU_TARGET), as
/// the rest of the scoring code does: VPOPCNTDQ counts 64-bit lanes of a
/// 512-bit register directly; AVX2 has no vector popcount, so bytes() counts
/// each nibble through a 16-entry shuffle table (Mula, Kurz and Lemire,
/// "Faster Population Counts Using AVX2 Instructions", 2018); NEON has a
/// per-byte count (cnt); anything else counts 64-bit words with the hardware
/// popcount (POPCNT on x86-64-v2).  CHUNK_BYTES is the vector width the
/// chunk functions work on, or 0 without one.
namespace luxir::popcnt {

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__)
inline constexpr size_t CHUNK_BYTES = 64;

/// Per 64-bit lane counts of x.
inline LUXIR_INLINE __m512i lanes(__m512i x) { return _mm512_popcnt_epi64(x); }
#elif defined(__AVX2__)
inline constexpr size_t CHUNK_BYTES = 32;

/// Per-byte counts of x (each 0..8).
inline LUXIR_INLINE __m256i bytes(__m256i x) {
  const __m256i table = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                         0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
  const __m256i low = _mm256_set1_epi8(0x0f);
  __m256i lo = _mm256_shuffle_epi8(table, _mm256_and_si256(x, low));
  __m256i hi = _mm256_shuffle_epi8(table, _mm256_and_si256(_mm256_srli_epi16(x, 4), low));
  return _mm256_add_epi8(lo, hi);
}

/// Per 64-bit lane sums of the bytes of x.
inline LUXIR_INLINE __m256i sumBytes(__m256i x) { return _mm256_sad_epu8(x, _mm256_setzero_si256()); }

/// Per 64-bit lane counts of x.
inline LUXIR_INLINE __m256i lanes(__m256i x) { return sumBytes(bytes(x)); }

/// Sum of the four 64-bit lanes of x.
inline LUXIR_INLINE uint64_t sum64(__m256i x) {
  __m128i s = _mm_add_epi64(_mm256_castsi256_si128(x), _mm256_extracti128_si256(x, 1));
  return (uint64_t)_mm_cvtsi128_si64(_mm_add_epi64(s, _mm_unpackhi_epi64(s, s)));
}
#elif defined(__ARM_NEON)
inline constexpr size_t CHUNK_BYTES = 16;

/// Per-byte counts of x (each 0..8).
inline LUXIR_INLINE uint8x16_t bytes(uint8x16_t x) { return vcntq_u8(x); }
#else
inline constexpr size_t CHUNK_BYTES = 0;
#endif

inline LUXIR_INLINE uint64_t load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

/// popcount(a ^ b) over n bytes, reading no byte past either range.
inline LUXIR_INLINE uint64_t hamming(const uint8_t* a, const uint8_t* b, size_t n) {
  uint64_t count = 0;
  size_t i = 0;
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__)
  // Zen 5, VPOPCNTDQ vs scalar in ns/row: 1.70 vs 0.94 at 8 bytes,
  // 1.78 vs 1.27 at 16, 1.88 vs 1.60 at 24, 1.73 vs 1.96 at 32,
  // and 2.25 vs 4.75 at 96. Use scalar below 32 bytes.
  if (n >= 32) {
    __m512i acc = _mm512_setzero_si512();
    for (; i + CHUNK_BYTES <= n; i += CHUNK_BYTES) {
      acc = _mm512_add_epi64(acc, lanes(_mm512_xor_si512(_mm512_loadu_si512(a + i), _mm512_loadu_si512(b + i))));
    }
    if (i < n) {
      // Masked loads read only the remaining bytes, never past the row.
      __mmask64 mask = ((uint64_t)1 << (n - i)) - 1;
      acc = _mm512_add_epi64(acc, lanes(_mm512_xor_si512(_mm512_maskz_loadu_epi8(mask, a + i),
                                                         _mm512_maskz_loadu_epi8(mask, b + i))));
      i = n;
    }
    count = (uint64_t)_mm512_reduce_add_epi64(acc);
  }
#elif defined(__AVX2__)
  // The nibble table pays for its setup and reduction from two chunks on:
  // below 64 bytes the 64-bit popcount loop is faster (25% at 32 bytes),
  // and from 128 bytes the table is 1.5x to 1.8x faster in cache.
  if (n >= 2 * CHUNK_BYTES) {
    __m256i acc = _mm256_setzero_si256();
    for (; i + CHUNK_BYTES <= n; i += CHUNK_BYTES) {
      acc = _mm256_add_epi64(acc, lanes(_mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i)),
                                                         _mm256_loadu_si256((const __m256i*)(b + i)))));
    }
    count = sum64(acc);
  }
#elif defined(__ARM_NEON)
  uint32x4_t acc = vdupq_n_u32(0);
  for (; i + CHUNK_BYTES <= n; i += CHUNK_BYTES) {
    acc = vpadalq_u16(acc, vpaddlq_u8(bytes(veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i)))));
  }
  count = vaddvq_u32(acc);
#endif
  for (; i + 8 <= n; i += 8) count += (uint64_t)std::popcount(load64(a + i) ^ load64(b + i));
  for (; i < n; i++) count += (uint64_t)std::popcount((uint8_t)(a[i] ^ b[i]));
  return count;
}

} // namespace luxir::popcnt
