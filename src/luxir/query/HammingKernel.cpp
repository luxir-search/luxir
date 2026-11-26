// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "HammingKernel.h"

#include <bit>
#include <cstring>

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
#define LUXIR_HAMMING_X86
#include <immintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#define LUXIR_HAMMING_NEON
#include <arm_neon.h>
#endif

namespace luxir::hamming {
namespace {

namespace words {

inline uint64_t load64(const uint8_t* p) {
  uint64_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

// Keep the word kernel scalar even when the build target has vector popcount.
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::optimize("no-tree-vectorize")]]
#endif
uint64_t count(const uint8_t* a, const uint8_t* b, size_t n) {
  uint64_t sum = 0;
#ifdef __clang__
  #pragma clang loop vectorize(disable) interleave(disable)
#endif
  for (; n >= 8; n -= 8, a += 8, b += 8) {
    sum += (uint64_t)std::popcount(load64(a) ^ load64(b));
  }
  for (; n != 0; --n, ++a, ++b) sum += (uint64_t)std::popcount((uint8_t)(*a ^ *b));
  return sum;
}

} // namespace words

#ifdef LUXIR_HAMMING_X86
#pragma GCC push_options
#pragma GCC target("avx2")
namespace avx2 {

uint64_t count(const uint8_t* a, const uint8_t* b, size_t n) {
  // Nibble popcounts, repeated in each 128-bit lane (Mula et al.).
  const __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                      0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
  const __m256i low = _mm256_set1_epi8(0x0f);
  const __m256i zero = _mm256_setzero_si256();
  __m256i acc = zero;
  for (; n >= 32; n -= 32, a += 32, b += 32) {
    __m256i x = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)a),
                                _mm256_loadu_si256((const __m256i*)b));
    __m256i lo = _mm256_shuffle_epi8(lut, _mm256_and_si256(x, low));
    __m256i hi = _mm256_shuffle_epi8(lut, _mm256_and_si256(_mm256_srli_epi16(x, 4), low));
    acc = _mm256_add_epi64(acc, _mm256_sad_epu8(_mm256_add_epi8(lo, hi), zero));
  }
  __m128i pair = _mm_add_epi64(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
  uint64_t sum = (uint64_t)_mm_cvtsi128_si64(_mm_add_epi64(pair, _mm_unpackhi_epi64(pair, pair)));
  return sum + (n != 0 ? words::count(a, b, n) : 0);
}

} // namespace avx2
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC target("avx2,avx512f,avx512bw,avx512vpopcntdq")
namespace avx512 {

uint64_t count(const uint8_t* a, const uint8_t* b, size_t n) {
  __m512i acc = _mm512_setzero_si512();
  for (; n >= 64; n -= 64, a += 64, b += 64) {
    __m512i x = _mm512_xor_si512(_mm512_loadu_si512(a), _mm512_loadu_si512(b));
    acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(x));
  }
  if (n != 0) {
    __mmask64 mask = ((uint64_t)1 << n) - 1;
    __m512i x = _mm512_xor_si512(_mm512_maskz_loadu_epi8(mask, a), _mm512_maskz_loadu_epi8(mask, b));
    acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(x));
  }
  return (uint64_t)_mm512_reduce_add_epi64(acc);
}

} // namespace avx512
#pragma GCC pop_options
#endif

#ifdef LUXIR_HAMMING_NEON
namespace neon {

uint64_t count(const uint8_t* a, const uint8_t* b, size_t n) {
  uint64x2_t acc = vdupq_n_u64(0);
  for (; n >= 16; n -= 16, a += 16, b += 16) {
    uint8x16_t bits = vcntq_u8(veorq_u8(vld1q_u8(a), vld1q_u8(b)));
    acc = vpadalq_u32(acc, vpaddlq_u16(vpaddlq_u8(bits)));
  }
  return vaddvq_u64(acc) + (n != 0 ? words::count(a, b, n) : 0);
}

} // namespace neon
#endif

constexpr Kernel levels[] = {
  {"words", words::count},
#ifdef LUXIR_HAMMING_X86
  {"avx2", avx2::count},
  {"avx512", avx512::count},
#elif defined(LUXIR_HAMMING_NEON)
  {"neon", neon::count},
#endif
};

} // namespace

std::span<const Kernel> supported() {
#ifdef LUXIR_HAMMING_X86
  static const auto available = [] {
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("popcnt")) {
      return std::span<const Kernel>(levels, 1);
    }
    bool wide = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")
        && __builtin_cpu_supports("avx512vpopcntdq");
    return std::span<const Kernel>(levels, wide ? 3 : 2);
  }();
  return available;
#else
  return levels;
#endif
}

KernelFn best(size_t rowBytes) {
#ifdef LUXIR_HAMMING_X86
  // Zen 5, median ns/row from cached contiguous rows. VPOPCNTDQ and words
  // tie at 16 and 24 bytes (~1.71). At 32 bytes they take 1.71 and 2.10,
  // and at 96 bytes 1.91 and 3.75. Use 32 bytes for a clear gain on
  // word-sized rows.
  constexpr size_t AVX512_MIN_BYTES = 32;
  // The AVX2 table beats words at exactly 32 bytes (1.91 vs 2.10), but
  // loses at 40 (2.70 vs 2.29). It wins consistently from 64 bytes
  // (1.91 vs 2.67), including rows with a word or byte tail.
  constexpr size_t AVX2_MIN_BYTES = 64;
  auto available = supported();
  if (available.size() == 3 && rowBytes >= AVX512_MIN_BYTES) return avx512::count;
  if (available.size() >= 2 && rowBytes >= AVX2_MIN_BYTES) return avx2::count;
#elif defined(LUXIR_HAMMING_NEON)
  // Below one full chunk the NEON kernel would only count the word tail.
  if (rowBytes >= 16) return neon::count;
#else
  (void)rowBytes;
#endif
  return words::count;
}

} // namespace luxir::hamming
