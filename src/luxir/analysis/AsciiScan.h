// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Bulk ASCII kernels for the analysis hot path: find the first non-ASCII byte
// and copy-with-ASCII-lowercase. Compile-time dispatch on the target ISA, like
// DecodedSuccessor.h: AVX2 when the build targets it, else SSE2 (always present
// on x86-64, so the x86-64-v2 container build gets it), NEON on arm64, and a
// portable 8-byte SWAR loop everywhere else. The *Swar variants are always
// compiled so tests can check every SIMD path against them on any host.

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__) || defined(__SSE2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace luxir::ascii {

namespace detail {

constexpr uint64_t kOnes = 0x0101010101010101ull;
constexpr uint64_t kHigh = 0x8080808080808080ull;

inline uint64_t load64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

inline void store64(char* p, uint64_t v) { std::memcpy(p, &v, 8); }

// 0x20 in every byte lane of an all-ASCII word that holds 'A'..'Z', else 0.
// Each lane stays below 0x80 before the adds, so no carry crosses lanes.
inline uint64_t upperMask64(uint64_t v) {
  uint64_t geA = v + (0x80 - 'A') * kOnes;      // high bit set iff byte >= 'A'
  uint64_t gtZ = v + (0x80 - 'Z' - 1) * kOnes;  // high bit set iff byte > 'Z'
  return ((geA & ~gtZ) & kHigh) >> 2;
}

inline size_t firstHighByteInWord(uint64_t hi) {
  // hi has only high bits set; the lowest set lane is the first in memory
  // order on little-endian, the highest on big-endian.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return (size_t) (__builtin_clzll(hi) >> 3);
#else
  return (size_t) (__builtin_ctzll(hi) >> 3);
#endif
}

}  // namespace detail

// Index of the first byte >= 0x80 in [p, p + n), or n if all ASCII. Portable.
inline size_t findNonAsciiSwar(const char* p, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t hi = detail::load64(p + i) & detail::kHigh;
    if (hi) return i + detail::firstHighByteInWord(hi);
  }
  for (; i < n; i++) {
    if ((unsigned char) p[i] & 0x80) return i;
  }
  return n;
}

// Copy n bytes from src to dst, mapping 'A'..'Z' to 'a'..'z' and leaving every
// other byte (including all bytes >= 0x80) unchanged. Portable.
inline void lowerCopySwar(char* dst, const char* src, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v = detail::load64(src + i);
    // Lanes >= 0x80 are masked out of the range test (they never match),
    // and keep their bytes because only matching lanes get +0x20.
    uint64_t ascii = ~v & detail::kHigh;
    uint64_t low7 = v & ~detail::kHigh;
    uint64_t mask = detail::upperMask64(low7) & (ascii >> 2);
    detail::store64(dst + i, v | mask);
  }
  for (; i < n; i++) {
    unsigned char c = (unsigned char) src[i];
    dst[i] = (char) (c - 'A' < 26u ? c + 32 : c);
  }
}

inline size_t findNonAscii(const char* p, size_t n) {
  size_t i = 0;
#if defined(__AVX2__)
  for (; i + 32 <= n; i += 32) {
    unsigned m = (unsigned) _mm256_movemask_epi8(_mm256_loadu_si256((const __m256i*) (p + i)));
    if (m) return i + (size_t) __builtin_ctz(m);
  }
#endif
#if defined(__SSE2__)
  for (; i + 16 <= n; i += 16) {
    unsigned m = (unsigned) _mm_movemask_epi8(_mm_loadu_si128((const __m128i*) (p + i)));
    if (m) return i + (size_t) __builtin_ctz(m);
  }
#elif defined(__ARM_NEON) && defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    uint8x16_t v = vld1q_u8((const uint8_t*) (p + i));
    if (vmaxvq_u8(v) >= 0x80) break;  // locate the byte below
  }
#endif
  return i + findNonAsciiSwar(p + i, n - i);
}

inline void lowerCopy(char* dst, const char* src, size_t n) {
  size_t i = 0;
#if defined(__AVX2__)
  {
    // Signed compares: bytes >= 0x80 are negative, so they fall outside
    // ['A', 'Z'] and are copied unchanged.
    const __m256i lo = _mm256_set1_epi8('A' - 1);
    const __m256i hi = _mm256_set1_epi8('Z' + 1);
    const __m256i bit = _mm256_set1_epi8(0x20);
    for (; i + 32 <= n; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i*) (src + i));
      __m256i up = _mm256_and_si256(_mm256_cmpgt_epi8(v, lo), _mm256_cmpgt_epi8(hi, v));
      _mm256_storeu_si256((__m256i*) (dst + i), _mm256_or_si256(v, _mm256_and_si256(up, bit)));
    }
  }
#endif
#if defined(__SSE2__)
  {
    const __m128i lo = _mm_set1_epi8('A' - 1);
    const __m128i hi = _mm_set1_epi8('Z' + 1);
    const __m128i bit = _mm_set1_epi8(0x20);
    for (; i + 16 <= n; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i*) (src + i));
      __m128i up = _mm_and_si128(_mm_cmpgt_epi8(v, lo), _mm_cmplt_epi8(v, hi));
      _mm_storeu_si128((__m128i*) (dst + i), _mm_or_si128(v, _mm_and_si128(up, bit)));
    }
  }
#elif defined(__ARM_NEON)
  {
    const uint8x16_t a = vdupq_n_u8('A');
    const uint8x16_t range = vdupq_n_u8(26);
    const uint8x16_t bit = vdupq_n_u8(0x20);
    for (; i + 16 <= n; i += 16) {
      uint8x16_t v = vld1q_u8((const uint8_t*) (src + i));
      uint8x16_t up = vcltq_u8(vsubq_u8(v, a), range);
      vst1q_u8((uint8_t*) (dst + i), vorrq_u8(v, vandq_u8(up, bit)));
    }
  }
#endif
  lowerCopySwar(dst + i, src + i, n - i);
}

}  // namespace luxir::ascii
