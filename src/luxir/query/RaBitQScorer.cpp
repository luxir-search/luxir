// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "RaBitQScorer.h"

#include "luxir/util/luxir_util.h"
#include <faiss/utils/distances.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__) || defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#elif defined(__SSE4_1__) || defined(__SSSE3__)
#include <smmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace luxir {
namespace {

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__)
constexpr size_t CHUNK_BYTES = 64;
constexpr bool MASKED_CHUNKS = true;
#else
constexpr bool MASKED_CHUNKS = false;
#if defined(__AVX2__)
constexpr size_t CHUNK_BYTES = 32;
#elif defined(__ARM_NEON)
constexpr size_t CHUNK_BYTES = 16;
#else
constexpr size_t CHUNK_BYTES = 0;  // Hardware popcount over 64-bit words.
#endif
#endif

constexpr size_t PREFETCH_ROWS = 6;

#if defined(__FMA__) || defined(__ARM_FEATURE_FMA)
constexpr bool FUSED = true;
#else
constexpr bool FUSED = false;
#endif

// The estimate from a row's integer counts and factors. With FMA, the two
// fused steps are the ones contraction gave this expression before the file
// was built with -ffp-contract=off; the block kernel's vector form matches.
template <bool IP>
inline LUXIR_INLINE float estimate(float dot, float sum, float orMinusC, float dpMultiplier,
                                   const RaBitQScorer::Factors& f) {
  float finalDot, preDist;
  if constexpr (FUSED) {
    finalDot = std::fma(f.c1, dot, f.c2 * sum) - f.c34;
    preDist = std::fma(-(2 * dpMultiplier), finalDot, orMinusC + f.queryToCentroidSqr);
  } else {
    finalDot = f.c1 * dot + f.c2 * sum - f.c34;
    preDist = orMinusC + f.queryToCentroidSqr - 2 * dpMultiplier * finalDot;
  }
  return IP ? -0.5f * (preDist - f.queryNormSqr) : preDist;
}

// Lookup tables of a block kernel step reach 4 * 15 per entry, so four of
// them add up in a byte. A u16 lane takes a segment of at most this many code
// bytes (two lanes fold into one row sum): 512 * 2 * 60 < 65536.
constexpr size_t SEGMENT_PAIRS = 512;

inline LUXIR_INLINE uint64_t load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Query values are finite (the parser rejects others), so the SIMD min and max
// see no NaN. A signed zero extreme can differ from std::min's pick, which only
// affects the sign of a zero c2 and so no estimate.
void minMax(const float* values, size_t n, float& low, float& high) {
  float minValue = values[0], maxValue = values[0];
  size_t i = 0;
#if defined(__AVX2__)
  if (n >= 8) {
    __m256 lo = _mm256_loadu_ps(values), hi = lo;
    for (i = 8; i + 8 <= n; i += 8) {
      __m256 v = _mm256_loadu_ps(values + i);
      lo = _mm256_min_ps(lo, v);
      hi = _mm256_max_ps(hi, v);
    }
    __m128 l = _mm_min_ps(_mm256_castps256_ps128(lo), _mm256_extractf128_ps(lo, 1));
    __m128 h = _mm_max_ps(_mm256_castps256_ps128(hi), _mm256_extractf128_ps(hi, 1));
    l = _mm_min_ps(l, _mm_movehl_ps(l, l));
    h = _mm_max_ps(h, _mm_movehl_ps(h, h));
    minValue = _mm_cvtss_f32(_mm_min_ss(l, _mm_shuffle_ps(l, l, 1)));
    maxValue = _mm_cvtss_f32(_mm_max_ss(h, _mm_shuffle_ps(h, h, 1)));
  }
#elif defined(__SSE4_1__)
  if (n >= 4) {
    __m128 lo = _mm_loadu_ps(values), hi = lo;
    for (i = 4; i + 4 <= n; i += 4) {
      __m128 v = _mm_loadu_ps(values + i);
      lo = _mm_min_ps(lo, v);
      hi = _mm_max_ps(hi, v);
    }
    lo = _mm_min_ps(lo, _mm_movehl_ps(lo, lo));
    hi = _mm_max_ps(hi, _mm_movehl_ps(hi, hi));
    minValue = _mm_cvtss_f32(_mm_min_ss(lo, _mm_shuffle_ps(lo, lo, 1)));
    maxValue = _mm_cvtss_f32(_mm_max_ss(hi, _mm_shuffle_ps(hi, hi, 1)));
  }
#elif defined(__ARM_NEON)
  if (n >= 4) {
    float32x4_t lo = vld1q_f32(values), hi = lo;
    for (i = 4; i + 4 <= n; i += 4) {
      float32x4_t v = vld1q_f32(values + i);
      lo = vminq_f32(lo, v);
      hi = vmaxq_f32(hi, v);
    }
    minValue = vminvq_f32(lo);
    maxValue = vmaxvq_f32(hi);
  }
#endif
  for (; i < n; i++) {
    minValue = std::min(minValue, values[i]);
    maxValue = std::max(maxValue, values[i]);
  }
  low = minValue;
  high = maxValue;
}

// Scorer definition: round (residual - low) / delta half up and clamp to the
// query-code range. Constant residuals map to zero (0 * inf becomes NaN, then
// the first comparison selects zero). The half test has no rounding of its own.
// Written to auto-vectorize: its compile-time target picks the width.
LUXIR_NOINLINE uint32_t quantize(const float* LUXIR_RESTRICT residual, size_t n, float low, float scale,
                                            float maxCode, uint8_t* LUXIR_RESTRICT out) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    float x = (residual[i] - low) * scale;
    x = x >= 0 ? x : 0;
    x = x < maxCode ? x : maxCode;
    int32_t truncated = (int32_t)x;
    int32_t code = truncated + (x - (float)truncated >= 0.5f ? 1 : 0);
    out[i] = (uint8_t)code;
    sum += (uint32_t)code;
  }
  return sum;
}

// Transposes 8 bytes and 8 bit planes: on return byte p holds bit p of each
// input byte, the input's first byte in the lowest bit.
inline LUXIR_INLINE uint64_t transposeBits(uint64_t x) {
  uint64_t t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL;
  x ^= t ^ (t << 7);
  t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL;
  x ^= t ^ (t << 14);
  t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL;
  return x ^ t ^ (t << 28);
}

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__)
struct ChunkOps {
  __mmask64 lastMask;

  explicit ChunkOps(size_t codeBytes)
      : lastMask(codeBytes % CHUNK_BYTES ? ((uint64_t)1 << (codeBytes % CHUNK_BYTES)) - 1 : ~(uint64_t)0) {}

  template <int QB>
  LUXIR_INLINE void score(const uint8_t* code, const uint8_t* planes, size_t chunks, uint64_t& dot, uint64_t& sum) const {
    const __m512i* plane = (const __m512i*)planes;
    __m512i weighted = _mm512_setzero_si512(), bits = _mm512_setzero_si512();
    auto add = [&](__m512i row) LUXIR_INLINE {
      __m512i acc = _mm512_setzero_si512();
      for (int p = QB - 1; p >= 0; p--) {
        __m512i count = _mm512_popcnt_epi64(_mm512_and_si512(row, _mm512_load_si512(plane + p)));
        acc = _mm512_add_epi64(_mm512_add_epi64(acc, acc), count);
      }
      weighted = _mm512_add_epi64(weighted, acc);
      bits = _mm512_add_epi64(bits, _mm512_popcnt_epi64(row));
      plane += QB;
    };
    // Mask only the last chunk, which can run into the factor bytes or past the
    // code: a masked load of every chunk costs 2x per code at 1536 dimensions.
    for (size_t c = 0; c + 1 < chunks; c++) add(_mm512_loadu_si512(code + c * CHUNK_BYTES));
    add(_mm512_maskz_loadu_epi8(lastMask, code + (chunks - 1) * CHUNK_BYTES));
    // One reduction for both: dot in the even lanes, sum in the odd.
    __m512i both = _mm512_add_epi64(_mm512_unpacklo_epi64(weighted, bits), _mm512_unpackhi_epi64(weighted, bits));
    __m256i half = _mm256_add_epi64(_mm512_castsi512_si256(both), _mm512_extracti64x4_epi64(both, 1));
    __m128i pair = _mm_add_epi64(_mm256_castsi256_si128(half), _mm256_extracti128_si256(half, 1));
    dot = (uint64_t)_mm_cvtsi128_si64(pair);
    sum = (uint64_t)_mm_extract_epi64(pair, 1);
  }
};
#elif defined(__AVX2__)
struct ChunkOps {
  __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
  __m256i low = _mm256_set1_epi8(0x0f);

  explicit ChunkOps(size_t) {}

  LUXIR_INLINE __m256i popBytes(__m256i x) const {
    __m256i lo = _mm256_shuffle_epi8(lut, _mm256_and_si256(x, low));
    __m256i hi = _mm256_shuffle_epi8(lut, _mm256_and_si256(_mm256_srli_epi16(x, 4), low));
    return _mm256_add_epi8(lo, hi);
  }
  static LUXIR_INLINE uint64_t sum64(__m256i x) {
    __m128i s = _mm_add_epi64(_mm256_castsi256_si128(x), _mm256_extracti128_si256(x, 1));
    return (uint64_t)_mm_cvtsi128_si64(_mm_add_epi64(s, _mm_unpackhi_epi64(s, s)));
  }
  // Weighted counts of the planes in bytes, most significant plane first. Four
  // planes of 8 bits weigh at most 8 * 15, which fits.
  LUXIR_INLINE __m256i horner(__m256i code, const __m256i* plane, int count) const {
    __m256i acc = _mm256_setzero_si256();
    for (int p = count - 1; p >= 0; p--) {
      __m256i bits = popBytes(_mm256_and_si256(code, _mm256_load_si256(plane + p)));
      acc = _mm256_add_epi8(_mm256_add_epi8(acc, acc), bits);
    }
    return _mm256_sad_epu8(acc, _mm256_setzero_si256());
  }
  template <int QB>
  LUXIR_INLINE void score(const uint8_t* code, const uint8_t* planes, size_t chunks, uint64_t& dot, uint64_t& sum) const {
    const __m256i* plane = (const __m256i*)planes;
    __m256i low4 = _mm256_setzero_si256(), high4 = _mm256_setzero_si256(), bits = _mm256_setzero_si256();
    for (size_t c = 0; c < chunks; c++, plane += QB) {
      __m256i row = _mm256_loadu_si256((const __m256i*)(code + c * CHUNK_BYTES));
      low4 = _mm256_add_epi64(low4, horner(row, plane, std::min(QB, 4)));
      if constexpr (QB > 4) high4 = _mm256_add_epi64(high4, horner(row, plane + 4, QB - 4));
      bits = _mm256_add_epi64(bits, _mm256_sad_epu8(popBytes(row), _mm256_setzero_si256()));
    }
    dot = sum64(low4);
    if constexpr (QB > 4) dot += sum64(high4) << 4;
    sum = sum64(bits);
  }
};
#elif defined(__ARM_NEON)
struct ChunkOps {
  explicit ChunkOps(size_t) {}

  static LUXIR_INLINE uint8x16_t horner(uint8x16_t code, const uint8_t* plane, int count) {
    uint8x16_t acc = vdupq_n_u8(0);
    for (int p = count - 1; p >= 0; p--) {
      uint8x16_t bits = vcntq_u8(vandq_u8(code, vld1q_u8(plane + (size_t)p * CHUNK_BYTES)));
      acc = vaddq_u8(vaddq_u8(acc, acc), bits);
    }
    return acc;
  }
  template <int QB>
  LUXIR_INLINE void score(const uint8_t* code, const uint8_t* plane, size_t chunks, uint64_t& dot, uint64_t& sum) const {
    uint32_t low4 = 0, high4 = 0, bits = 0;
    for (size_t c = 0; c < chunks; c++, plane += QB * CHUNK_BYTES) {
      uint8x16_t row = vld1q_u8(code + c * CHUNK_BYTES);
      low4 += vaddlvq_u8(horner(row, plane, std::min(QB, 4)));
      if constexpr (QB > 4) high4 += vaddlvq_u8(horner(row, plane + 4 * CHUNK_BYTES, QB - 4));
      bits += vaddlvq_u8(vcntq_u8(row));
    }
    dot = (uint64_t)low4 + ((uint64_t)high4 << 4);
    sum = bits;
  }
};
#else
struct ChunkOps {
  explicit ChunkOps(size_t) {}

  template <int QB>
  LUXIR_INLINE void score(const uint8_t*, const uint8_t*, size_t, uint64_t& dot, uint64_t& sum) const {
    dot = 0;
    sum = 0;
  }
};
#endif


// Index vectors that broadcast and mask the quantized query in one shuffle:
// entry v of group h picks dimension 4h + k where bit k of v is set, and an
// index with the high bit set yields zero (pshufb and tbl alike).
struct TableSelect {
  alignas(32) uint8_t index[4][32];
};
constexpr TableSelect makeTableSelect() {
  TableSelect s{};
  for (int k = 0; k < 4; k++)
    for (int h = 0; h < 2; h++)
      for (int v = 0; v < 16; v++) s.index[k][16 * h + v] = (v >> k) & 1 ? (uint8_t)(4 * h + k) : 0x80;
  return s;
}
constexpr TableSelect TABLE_SELECT = makeTableSelect();

// Subset sums of each 4-dimension group: 32 bytes per code byte p, [group 2p
// | group 2p + 1], entry v the sum of the quantized values of the group's
// dimensions whose bit is set in v. Query bits 1 to 4, so entries fit a byte.
void buildBlockTables(const uint8_t* quantized, size_t codeBytes, uint8_t* tables) {
#if defined(__AVX2__)
  __m256i select[4];
  for (int k = 0; k < 4; k++) select[k] = _mm256_load_si256((const __m256i*)TABLE_SELECT.index[k]);
  for (size_t p = 0; p < codeBytes; p++) {
    __m256i q = _mm256_set1_epi64x((long long)load64(quantized + 8 * p));
    __m256i t = _mm256_add_epi8(
        _mm256_add_epi8(_mm256_shuffle_epi8(q, select[0]), _mm256_shuffle_epi8(q, select[1])),
        _mm256_add_epi8(_mm256_shuffle_epi8(q, select[2]), _mm256_shuffle_epi8(q, select[3])));
    _mm256_storeu_si256((__m256i*)(tables + 32 * p), t);
  }
#elif defined(__SSSE3__)
  __m128i select[2][4];
  for (int h = 0; h < 2; h++)
    for (int k = 0; k < 4; k++) select[h][k] = _mm_load_si128((const __m128i*)(TABLE_SELECT.index[k] + 16 * h));
  for (size_t p = 0; p < codeBytes; p++) {
    __m128i q = _mm_set1_epi64x((long long)load64(quantized + 8 * p));
    for (int h = 0; h < 2; h++) {
      __m128i t = _mm_add_epi8(_mm_add_epi8(_mm_shuffle_epi8(q, select[h][0]), _mm_shuffle_epi8(q, select[h][1])),
                               _mm_add_epi8(_mm_shuffle_epi8(q, select[h][2]), _mm_shuffle_epi8(q, select[h][3])));
      _mm_storeu_si128((__m128i*)(tables + 32 * p + 16 * h), t);
    }
  }
#elif defined(__ARM_NEON) && defined(__aarch64__)
  for (size_t p = 0; p < codeBytes; p++) {
    uint8x8_t q8 = vld1_u8(quantized + 8 * p);
    uint8x16_t q = vcombine_u8(q8, q8);
    for (int h = 0; h < 2; h++) {
      uint8x16_t t = vqtbl1q_u8(q, vld1q_u8(TABLE_SELECT.index[0] + 16 * h));
      for (int k = 1; k < 4; k++) t = vaddq_u8(t, vqtbl1q_u8(q, vld1q_u8(TABLE_SELECT.index[k] + 16 * h)));
      vst1q_u8(tables + 32 * p + 16 * h, t);
    }
  }
#else
  for (size_t p = 0; p < codeBytes; p++)
    for (int h = 0; h < 2; h++)
      for (int v = 0; v < 16; v++) {
        uint32_t sum = 0;
        for (int k = 0; k < 4; k++) if ((v >> k) & 1) sum += quantized[8 * p + 4 * h + k];
        tables[32 * p + 16 * h + v] = (uint8_t)sum;
      }
#endif
}

// Integer dots of the 32 rows of a block with the query tables.
void blockDotsImpl(const uint8_t* nibbles, const uint8_t* tables, size_t pairs, uint32_t* dots) {
#if defined(__AVX2__)
  const __m256i low = _mm256_set1_epi8(0x0f), even = _mm256_set1_epi16(0x00ff), zero = _mm256_setzero_si256();
  __m256i total[4] = {zero, zero, zero, zero};
  for (size_t begin = 0; begin < pairs; begin += SEGMENT_PAIRS) {
    size_t end = std::min(pairs, begin + SEGMENT_PAIRS);
    // u16 sums by even and odd byte: lane 0 has group 2p, lane 1 group 2p + 1.
    __m256i loEven = zero, loOdd = zero, hiEven = zero, hiOdd = zero;
    for (size_t p = begin; p < end;) {
      size_t stop = std::min(end, p + 4);
      __m256i lo = zero, hi = zero;
      for (; p < stop; p++) {
        __m256i x = _mm256_loadu_si256((const __m256i*)(nibbles + 32 * p));
        __m256i t = _mm256_loadu_si256((const __m256i*)(tables + 32 * p));
        lo = _mm256_add_epi8(lo, _mm256_shuffle_epi8(t, _mm256_and_si256(x, low)));
        hi = _mm256_add_epi8(hi, _mm256_shuffle_epi8(t, _mm256_and_si256(_mm256_srli_epi16(x, 4), low)));
      }
      loEven = _mm256_add_epi16(loEven, _mm256_and_si256(lo, even));
      loOdd = _mm256_add_epi16(loOdd, _mm256_srli_epi16(lo, 8));
      hiEven = _mm256_add_epi16(hiEven, _mm256_and_si256(hi, even));
      hiOdd = _mm256_add_epi16(hiOdd, _mm256_srli_epi16(hi, 8));
    }
    // Fold the two groups, then interleave even and odd rows into row order.
    auto fold = [](__m256i v) LUXIR_INLINE {
      return _mm_add_epi16(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    };
    __m128i le = fold(loEven), lo = fold(loOdd), he = fold(hiEven), ho = fold(hiOdd);
    __m128i rows[4] = {_mm_unpacklo_epi16(le, lo), _mm_unpackhi_epi16(le, lo),
                       _mm_unpacklo_epi16(he, ho), _mm_unpackhi_epi16(he, ho)};
    for (int k = 0; k < 4; k++) total[k] = _mm256_add_epi32(total[k], _mm256_cvtepu16_epi32(rows[k]));
  }
  for (int k = 0; k < 4; k++) _mm256_storeu_si256((__m256i*)(dots + 8 * k), total[k]);
#elif defined(__SSSE3__)
  // The NEON scheme on 16-byte vectors: both halves of two code bytes add to
  // one byte (4 * 60), then widen to u16 rows 0-7, 8-15, 16-23 and 24-31.
  const __m128i low = _mm_set1_epi8(0x0f), zero = _mm_setzero_si128();
  __m128i total[8];
  for (auto& t : total) t = zero;
  for (size_t begin = 0; begin < pairs; begin += SEGMENT_PAIRS) {
    size_t end = std::min(pairs, begin + SEGMENT_PAIRS);
    __m128i acc[4] = {zero, zero, zero, zero};
    for (size_t p = begin; p < end;) {
      size_t stop = std::min(end, p + 2);
      __m128i lo = zero, hi = zero;
      for (; p < stop; p++) {
        for (int h = 0; h < 2; h++) {
          __m128i x = _mm_loadu_si128((const __m128i*)(nibbles + 32 * p + 16 * h));
          __m128i t = _mm_loadu_si128((const __m128i*)(tables + 32 * p + 16 * h));
          lo = _mm_add_epi8(lo, _mm_shuffle_epi8(t, _mm_and_si128(x, low)));
          hi = _mm_add_epi8(hi, _mm_shuffle_epi8(t, _mm_and_si128(_mm_srli_epi16(x, 4), low)));
        }
      }
      acc[0] = _mm_add_epi16(acc[0], _mm_unpacklo_epi8(lo, zero));
      acc[1] = _mm_add_epi16(acc[1], _mm_unpackhi_epi8(lo, zero));
      acc[2] = _mm_add_epi16(acc[2], _mm_unpacklo_epi8(hi, zero));
      acc[3] = _mm_add_epi16(acc[3], _mm_unpackhi_epi8(hi, zero));
    }
    for (int k = 0; k < 4; k++) {
      total[2 * k] = _mm_add_epi32(total[2 * k], _mm_unpacklo_epi16(acc[k], zero));
      total[2 * k + 1] = _mm_add_epi32(total[2 * k + 1], _mm_unpackhi_epi16(acc[k], zero));
    }
  }
  for (int k = 0; k < 8; k++) _mm_storeu_si128((__m128i*)(dots + 4 * k), total[k]);
#elif defined(__ARM_NEON) && defined(__aarch64__)
  const uint8x16_t low = vdupq_n_u8(0x0f);
  uint32x4_t total[8];
  for (auto& t : total) t = vdupq_n_u32(0);
  for (size_t begin = 0; begin < pairs; begin += SEGMENT_PAIRS) {
    size_t end = std::min(pairs, begin + SEGMENT_PAIRS);
    // Rows 0-7, 8-15, 16-23 and 24-31.
    uint16x8_t acc[4] = {vdupq_n_u16(0), vdupq_n_u16(0), vdupq_n_u16(0), vdupq_n_u16(0)};
    for (size_t p = begin; p < end;) {
      // Both halves add to one byte: two code bytes per step.
      size_t stop = std::min(end, p + 2);
      uint8x16_t lo = vdupq_n_u8(0), hi = vdupq_n_u8(0);
      for (; p < stop; p++) {
        for (int h = 0; h < 2; h++) {
          uint8x16_t x = vld1q_u8(nibbles + 32 * p + 16 * h);
          uint8x16_t t = vld1q_u8(tables + 32 * p + 16 * h);
          lo = vaddq_u8(lo, vqtbl1q_u8(t, vandq_u8(x, low)));
          hi = vaddq_u8(hi, vqtbl1q_u8(t, vshrq_n_u8(x, 4)));
        }
      }
      acc[0] = vaddw_u8(acc[0], vget_low_u8(lo));
      acc[1] = vaddw_high_u8(acc[1], lo);
      acc[2] = vaddw_u8(acc[2], vget_low_u8(hi));
      acc[3] = vaddw_high_u8(acc[3], hi);
    }
    for (int k = 0; k < 4; k++) {
      total[2 * k] = vaddw_u16(total[2 * k], vget_low_u16(acc[k]));
      total[2 * k + 1] = vaddw_high_u16(total[2 * k + 1], acc[k]);
    }
  }
  for (int k = 0; k < 8; k++) vst1q_u32(dots + 4 * k, total[k]);
#else
  for (size_t j = 0; j < RaBitQBlockLayout::ROWS; j++) {
    size_t column = j % 16, shift = j < 16 ? 0 : 4;
    uint32_t dot = 0;
    for (size_t p = 0; p < pairs; p++)
      for (size_t h = 0; h < 2; h++) {
        const uint8_t* half = nibbles + 32 * p + 16 * h;
        dot += tables[32 * p + 16 * h + ((half[column] >> shift) & 0x0f)];
      }
    dots[j] = dot;
  }
#endif
}

// Estimates of the 32 rows of a block, in the float order of estimate().
template <bool IP>
void combineBlock(const uint32_t* dots, const uint8_t* block, const RaBitQBlockLayout& layout,
                  const RaBitQScorer::Factors& f, float* LUXIR_RESTRICT out) {
  const uint8_t* orMinusC = block + layout.orMinusCOffset();
  const uint8_t* dpMultiplier = block + layout.dpMultiplierOffset();
  const uint8_t* popcount = block + layout.popcountOffset();
#if defined(__AVX2__)
  const __m256 c1 = _mm256_set1_ps(f.c1), c2 = _mm256_set1_ps(f.c2), c34 = _mm256_set1_ps(f.c34);
  const __m256 toCentroid = _mm256_set1_ps(f.queryToCentroidSqr), norm = _mm256_set1_ps(f.queryNormSqr);
  for (size_t k = 0; k < RaBitQBlockLayout::ROWS; k += 8) {
    __m256 dot = _mm256_cvtepi32_ps(_mm256_loadu_si256((const __m256i*)(dots + k)));
    __m256 sum = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(
        _mm_loadu_si128((const __m128i*)(popcount + k * sizeof(uint16_t)))));
    __m256 dp = _mm256_loadu_ps((const float*)(dpMultiplier + k * sizeof(float)));
    __m256 base = _mm256_add_ps(_mm256_loadu_ps((const float*)(orMinusC + k * sizeof(float))), toCentroid);
    __m256 twice = _mm256_add_ps(dp, dp);
#if defined(__FMA__)
    __m256 finalDot = _mm256_sub_ps(_mm256_fmadd_ps(c1, dot, _mm256_mul_ps(c2, sum)), c34);
    __m256 preDist = _mm256_fnmadd_ps(twice, finalDot, base);
#else
    __m256 finalDot = _mm256_sub_ps(_mm256_add_ps(_mm256_mul_ps(c1, dot), _mm256_mul_ps(c2, sum)), c34);
    __m256 preDist = _mm256_sub_ps(base, _mm256_mul_ps(twice, finalDot));
#endif
    if constexpr (IP) preDist = _mm256_mul_ps(_mm256_set1_ps(-0.5f), _mm256_sub_ps(preDist, norm));
    _mm256_storeu_ps(out + k, preDist);
  }
#elif defined(__SSSE3__)
  const __m128 c1 = _mm_set1_ps(f.c1), c2 = _mm_set1_ps(f.c2), c34 = _mm_set1_ps(f.c34);
  const __m128 toCentroid = _mm_set1_ps(f.queryToCentroidSqr), norm = _mm_set1_ps(f.queryNormSqr);
  const __m128i zero = _mm_setzero_si128();
  for (size_t k = 0; k < RaBitQBlockLayout::ROWS; k += 4) {
    __m128 dot = _mm_cvtepi32_ps(_mm_loadu_si128((const __m128i*)(dots + k)));
    __m128 sum = _mm_cvtepi32_ps(_mm_unpacklo_epi16(
        _mm_loadl_epi64((const __m128i*)(popcount + k * sizeof(uint16_t))), zero));
    __m128 dp = _mm_loadu_ps((const float*)(dpMultiplier + k * sizeof(float)));
    __m128 base = _mm_add_ps(_mm_loadu_ps((const float*)(orMinusC + k * sizeof(float))), toCentroid);
    __m128 twice = _mm_add_ps(dp, dp);
#if defined(__FMA__)
    __m128 finalDot = _mm_sub_ps(_mm_fmadd_ps(c1, dot, _mm_mul_ps(c2, sum)), c34);
    __m128 preDist = _mm_fnmadd_ps(twice, finalDot, base);
#else
    __m128 finalDot = _mm_sub_ps(_mm_add_ps(_mm_mul_ps(c1, dot), _mm_mul_ps(c2, sum)), c34);
    __m128 preDist = _mm_sub_ps(base, _mm_mul_ps(twice, finalDot));
#endif
    if constexpr (IP) preDist = _mm_mul_ps(_mm_set1_ps(-0.5f), _mm_sub_ps(preDist, norm));
    _mm_storeu_ps(out + k, preDist);
  }
#elif defined(__ARM_NEON) && defined(__aarch64__)
  const float32x4_t c1 = vdupq_n_f32(f.c1), c2 = vdupq_n_f32(f.c2), c34 = vdupq_n_f32(f.c34);
  const float32x4_t toCentroid = vdupq_n_f32(f.queryToCentroidSqr), norm = vdupq_n_f32(f.queryNormSqr);
  for (size_t k = 0; k < RaBitQBlockLayout::ROWS; k += 4) {
    float32x4_t dot = vcvtq_f32_u32(vld1q_u32(dots + k));
    float32x4_t sum = vcvtq_f32_u32(vmovl_u16(vld1_u16((const uint16_t*)(popcount + k * sizeof(uint16_t)))));
    float32x4_t dp = vld1q_f32((const float*)(dpMultiplier + k * sizeof(float)));
    float32x4_t base = vaddq_f32(vld1q_f32((const float*)(orMinusC + k * sizeof(float))), toCentroid);
    float32x4_t finalDot = vsubq_f32(vfmaq_f32(vmulq_f32(c2, sum), c1, dot), c34);
    float32x4_t preDist = vfmsq_f32(base, vaddq_f32(dp, dp), finalDot);
    if constexpr (IP) preDist = vmulq_f32(vdupq_n_f32(-0.5f), vsubq_f32(preDist, norm));
    vst1q_f32(out + k, preDist);
  }
#else
  for (size_t k = 0; k < RaBitQBlockLayout::ROWS; k++) {
    float base, dp;
    uint16_t bits;
    std::memcpy(&base, orMinusC + k * sizeof(float), sizeof(float));
    std::memcpy(&dp, dpMultiplier + k * sizeof(float), sizeof(float));
    std::memcpy(&bits, popcount + k * sizeof(uint16_t), sizeof(bits));
    out[k] = estimate<IP>((float)(int64_t)dots[k], (float)bits, base, dp, f);
  }
#endif
}

}  // namespace

void RaBitQBlockLayout::pack(const uint8_t* codes, size_t codeSize, size_t count, uint8_t* out) const {
  assert(count <= ROWS);
  std::memset(out, 0, bytes);
  auto nibble = [&](size_t row, size_t p, size_t h) -> uint8_t {
    return row < count ? (codes[row * codeSize + p] >> (4 * h)) & 0x0f : 0;
  };
  for (size_t p = 0; p < codeBytes; p++)
    for (size_t h = 0; h < 2; h++)
      for (size_t j = 0; j < 16; j++)
        out[32 * p + 16 * h + j] = (uint8_t)(nibble(j, p, h) | nibble(j + 16, p, h) << 4);
  for (size_t row = 0; row < count; row++) {
    const uint8_t* code = codes + row * codeSize;
    uint32_t bits = 0;
    for (size_t p = 0; p < codeBytes; p++) bits += (uint32_t)std::popcount((unsigned)code[p]);
    auto popcount = (uint16_t)bits;
    std::memcpy(out + orMinusCOffset() + row * sizeof(float), code + codeBytes, sizeof(float));
    std::memcpy(out + dpMultiplierOffset() + row * sizeof(float), code + codeBytes + sizeof(float), sizeof(float));
    std::memcpy(out + popcountOffset() + row * sizeof(uint16_t), &popcount, sizeof(popcount));
  }
}

bool RaBitQBlockLayout::bit(const uint8_t* block, size_t row, size_t dim) const {
  uint8_t byte = block[32 * (dim / 8) + 16 * ((dim / 4) % 2) + row % 16];
  return ((row < 16 ? byte : byte >> 4) >> (dim % 4)) & 1;
}

RaBitQScorer::RaBitQScorer(int32_t dims, int32_t queryBits, bool innerProduct, const float* query, bool blocks)
    : query(query), dims((size_t)dims), codeBytes(((size_t)dims + 7) / 8),
      chunks(CHUNK_BYTES == 0 ? 0 : (codeBytes + (MASKED_CHUNKS ? CHUNK_BYTES - 1 : 0))
          / std::max(CHUNK_BYTES, (size_t)1)),
      tailWords(MASKED_CHUNKS ? 0 : (codeBytes - chunks * CHUNK_BYTES + 7) / 8),
      queryBits(queryBits), innerProduct(innerProduct), useBlocks(blocks && blockQueryBits(queryBits)),
      invSqrtDims(1.0f / std::sqrt((float)dims)), maxCode((float)((1 << queryBits) - 1)), layout((size_t)dims) {
  assert(dims > 0 && queryBits >= 1 && queryBits <= 8);
  assert(!blocks || RaBitQBlockLayout::supports(dims));
  constexpr size_t ALIGN = 64;
  size_t residualBytes = (this->dims * sizeof(float) + ALIGN - 1) & ~(ALIGN - 1);
  // Whole 64-bit groups, so packPlanes can load past the last dimension.
  size_t quantizedBytes = (this->dims + 7 + ALIGN - 1) & ~(ALIGN - 1);
  size_t planeBytes = ((chunks * CHUNK_BYTES + tailWords * 8) * (size_t)queryBits + ALIGN - 1) & ~(ALIGN - 1);
  size_t tableBytes = useBlocks ? codeBytes * RaBitQBlockLayout::ROWS : 0;
  storage = std::make_unique_for_overwrite<uint8_t[]>(residualBytes + quantizedBytes + planeBytes + tableBytes + ALIGN);
  uint8_t* base = storage.get() + (ALIGN - (uintptr_t)storage.get() % ALIGN) % ALIGN;
  residual = (float*)base;
  quantized = base + residualBytes;
  planes = quantized + quantizedBytes;
  tables = planes + planeBytes;
  std::memset(quantized, 0, quantizedBytes + planeBytes + tableBytes);
  if (innerProduct) factors.queryNormSqr = faiss::fvec_norm_L2sqr(query, this->dims);
}

// FAISS packs one bit at a time; this takes 8, 32 or 64 dimensions at once.
void RaBitQScorer::packPlanes() const {
  const uint8_t* in = quantized;
  uint8_t* out = planes;
  size_t qb = (size_t)queryBits;
  auto packUnit = [&](size_t unit, size_t dimsLeft) {
    for (size_t byte = 0; byte < unit; byte++, in += 8) {
      if (byte * 8 >= dimsLeft) {
        for (size_t p = 0; p < qb; p++) out[p * unit + byte] = 0;
        continue;
      }
      uint64_t x = transposeBits(load64(in));
      for (size_t p = 0; p < qb; p++, x >>= 8) out[p * unit + byte] = (uint8_t)x;
    }
    out += qb * unit;
  };
  size_t left = dims;
  for (size_t c = 0; c < chunks; c++, left = left > CHUNK_BYTES * 8 ? left - CHUNK_BYTES * 8 : 0) {
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__)
    // Only load groups that contain dimensions. Quantized is padded to 64
    // dimensions, while the planes are padded to a whole 512-dimension chunk.
    for (size_t batch = 0; batch < CHUNK_BYTES && batch * 8 < left; batch += 8, in += 64) {
      __m512i q = _mm512_loadu_si512(in);
      for (size_t p = 0; p < qb; p++) {
        uint64_t mask = (uint64_t)_mm512_movepi8_mask(_mm512_slli_epi16(q, (int)(7 - p)));
        std::memcpy(out + p * CHUNK_BYTES + batch, &mask, sizeof(mask));
      }
    }
    out += qb * CHUNK_BYTES;
#elif defined(__AVX2__)
    for (size_t batch = 0; batch < CHUNK_BYTES; batch += 4, in += 32) {
      __m256i q = _mm256_loadu_si256((const __m256i*)in);
      for (size_t p = 0; p < qb; p++) {
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_slli_epi16(q, (int)(7 - p)));
        std::memcpy(out + p * CHUNK_BYTES + batch, &mask, sizeof(mask));
      }
    }
    out += qb * CHUNK_BYTES;
#else
    packUnit(CHUNK_BYTES, left);
#endif
  }
  for (size_t w = 0; w < tailWords; w++, left = left > 64 ? left - 64 : 0) packUnit(8, left);
}

uint8_t RaBitQScorer::planeByte(int32_t plane, size_t index) const {
  prepare(ROW_KERNEL);
  size_t qb = (size_t)queryBits;
  size_t chunkBytes = chunks * CHUNK_BYTES;
  if constexpr (CHUNK_BYTES != 0) {
    if (index < chunkBytes)
      return planes[(index / CHUNK_BYTES) * qb * CHUNK_BYTES + (size_t)plane * CHUNK_BYTES + index % CHUNK_BYTES];
  }
  index -= chunkBytes;
  return planes[chunkBytes * qb + (index / 8) * qb * 8 + (size_t)plane * 8 + index % 8];
}

void RaBitQScorer::setList(const float* centroid, int kernels) {
  factors.queryToCentroidSqr = faiss::fvec_L2sqr(query, centroid, dims);
  gError = std::sqrt(factors.queryToCentroidSqr);
  float* LUXIR_RESTRICT r = residual;
  const float* LUXIR_RESTRICT q = query;
  const float* LUXIR_RESTRICT c = centroid;
  for (size_t i = 0; i < dims; i++) r[i] = q[i] - c[i];
  float low, high;
  minMax(residual, dims, low, high);
  float delta = (high - low) / maxCode;
  uint32_t sum = quantize(residual, dims, low, 1.0f / delta, maxCode, quantized);
  prepared = 0;
  prepare(useBlocks ? kernels : kernels & ROW_KERNEL);
  factors.c1 = 2 * delta * invSqrtDims;
  factors.c2 = 2 * low * invSqrtDims;
  if constexpr (FUSED) factors.c34 = invSqrtDims * std::fma(delta, (float)sum, (float)dims * low);
  else factors.c34 = invSqrtDims * (delta * (float)sum + (float)dims * low);
}

template <int QB, class Ops>
inline LUXIR_INLINE RaBitQScorer::Counts RaBitQScorer::countCode(const uint8_t* code, const Ops& ops) const {
  uint64_t dot = 0, sum = 0;
  if constexpr (CHUNK_BYTES != 0) {
    if (chunks != 0) ops.template score<QB>(code, planes, chunks, dot, sum);
  }
  if constexpr (!MASKED_CHUNKS) {
    // Only the last word can overlap the factor bytes. Planes are zero there.
    const uint64_t lastMask = codeBytes % 8 == 0 ? ~(uint64_t)0 : ((uint64_t)1 << (codeBytes % 8 * 8)) - 1;
    const uint8_t* plane = planes + chunks * CHUNK_BYTES * QB;
    for (size_t w = 0; w < tailWords; w++, plane += QB * sizeof(uint64_t)) {
      uint64_t row = load64(code + chunks * CHUNK_BYTES + w * sizeof(uint64_t));
      for (int p = 0; p < QB; p++)
        dot += (uint64_t)std::popcount(row & load64(plane + (size_t)p * sizeof(uint64_t))) << p;
      sum += (uint64_t)std::popcount(w + 1 == tailWords ? row & lastMask : row);
    }
  }
  return {dot, sum};
}

RaBitQScorer::Counts RaBitQScorer::counts(const uint8_t* code) const {
  prepare(ROW_KERNEL);
  const ChunkOps ops(codeBytes);
  switch (queryBits) {
    case 1: return countCode<1>(code, ops);
    case 2: return countCode<2>(code, ops);
    case 3: return countCode<3>(code, ops);
    case 4: return countCode<4>(code, ops);
    case 5: return countCode<5>(code, ops);
    case 6: return countCode<6>(code, ops);
    case 7: return countCode<7>(code, ops);
    default: return countCode<8>(code, ops);
  }
}

template <int QB, bool IP, bool Selected>
LUXIR_NOINLINE void RaBitQScorer::scoreBlock(const uint8_t* codes, size_t codeSize, const uint16_t* rows,
                                             size_t count, float* LUXIR_RESTRICT out) const {
  const ChunkOps ops(codeBytes);
  const size_t binary = codeBytes;
  const Factors f = factors;
  for (size_t i = 0; i < count; i++) {
    const uint8_t* code = codes + (Selected ? (size_t)rows[i] : i) * codeSize;
    // Filtered rows skip unevenly, out of the hardware prefetcher's reach.
    if constexpr (Selected) {
      if (i + PREFETCH_ROWS < count) {
        const uint8_t* next = codes + (size_t)rows[i + PREFETCH_ROWS] * codeSize;
        __builtin_prefetch(next);
        __builtin_prefetch(next + codeSize - 1);
      }
    }
    auto [dot, sum] = countCode<QB>(code, ops);
    float orMinusC, dpMultiplier;
    std::memcpy(&orMinusC, code + binary, sizeof(float));
    std::memcpy(&dpMultiplier, code + binary + sizeof(float), sizeof(float));
    // Through int64: unsigned 64-bit to float is slow without AVX-512.
    out[i] = estimate<IP>((float)(int64_t)dot, (float)(int64_t)sum, orMinusC, dpMultiplier, f);
  }
}

template <bool Selected>
void RaBitQScorer::dispatch(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const {
  prepare(ROW_KERNEL);
  auto run = [&]<int QB>() {
    if (innerProduct) scoreBlock<QB, true, Selected>(codes, codeSize, rows, count, out);
    else scoreBlock<QB, false, Selected>(codes, codeSize, rows, count, out);
  };
  switch (queryBits) {
    case 1: run.template operator()<1>(); break;
    case 2: run.template operator()<2>(); break;
    case 3: run.template operator()<3>(); break;
    case 4: run.template operator()<4>(); break;
    case 5: run.template operator()<5>(); break;
    case 6: run.template operator()<6>(); break;
    case 7: run.template operator()<7>(); break;
    default: run.template operator()<8>(); break;
  }
}

void RaBitQScorer::score(const uint8_t* codes, size_t codeSize, size_t count, float* out) const {
  dispatch<false>(codes, codeSize, nullptr, count, out);
}

void RaBitQScorer::score(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const {
  dispatch<true>(codes, codeSize, rows, count, out);
}

void RaBitQScorer::buildTables() const {
  buildBlockTables(quantized, codeBytes, tables);
}

void RaBitQScorer::blockDots(const uint8_t* block, uint32_t* dots) const {
  assert(useBlocks);
  prepare(BLOCK_KERNEL);
  blockDotsImpl(block, tables, codeBytes, dots);
}

template <bool IP>
LUXIR_NOINLINE void RaBitQScorer::scoreBlocksImpl(const uint8_t* blocks, size_t count, float* out) const {
  alignas(32) uint32_t dots[RaBitQBlockLayout::ROWS];
  const Factors f = factors;
  for (size_t b = 0, n = RaBitQBlockLayout::blocks(count); b < n; b++) {
    const uint8_t* block = blocks + b * layout.bytes;
    blockDotsImpl(block, tables, codeBytes, dots);
    combineBlock<IP>(dots, block, layout, f, out + b * RaBitQBlockLayout::ROWS);
  }
}

void RaBitQScorer::scoreBlocks(const uint8_t* blocks, size_t count, float* out) const {
  assert(useBlocks);
  prepare(BLOCK_KERNEL);
  if (innerProduct) scoreBlocksImpl<true>(blocks, count, out);
  else scoreBlocksImpl<false>(blocks, count, out);
}

RaBitQScorer::BlockWork RaBitQScorer::score(const uint8_t* codes, size_t codeSize, const uint8_t* blocks,
                                            const uint16_t* rows, size_t count, float* out) const {
  assert(count <= BLOCK_ROWS);
  constexpr size_t ROWS = RaBitQBlockLayout::ROWS;
  const size_t minRows = blockMinRows((int32_t)dims);
  if (count < minRows) {
    // No block can be dense.
    if (count != 0) score(codes, codeSize, rows, count, out);
    return {};
  }
  // Rows ascend: the last row of a block stores where the block ends, a
  // store per selected row and no branch; blocks without rows then take the
  // end of the block before.
  uint16_t starts[BLOCK_ROWS / ROWS + 1] = {};
  for (size_t i = 0; i < count; i++) starts[rows[i] / ROWS + 1] = (uint16_t)(i + 1);
  const size_t blockCount = rows[count - 1] / ROWS + 1;
  for (size_t b = 1; b <= blockCount; b++) starts[b] = std::max(starts[b], starts[b - 1]);
  return score(codes, codeSize, blocks, rows, starts, blockCount, out);
}

RaBitQScorer::BlockWork RaBitQScorer::score(const uint8_t* codes, size_t codeSize, const uint8_t* blocks,
                                            const uint16_t* rows, const uint16_t* blockStarts, size_t blockCount,
                                            float* out) const {
  assert(useBlocks && blockCount <= BLOCK_ROWS / RaBitQBlockLayout::ROWS);
  constexpr size_t ROWS = RaBitQBlockLayout::ROWS;
  const size_t minRows = blockMinRows((int32_t)dims);
  const size_t count = blockStarts[blockCount];
  BlockWork work;
  if (count == 0) return work;
  uint32_t dense = 0;
  if (count >= minRows) {
    for (size_t b = 0; b < blockCount; b++)
      dense |= (uint32_t)((size_t)(blockStarts[b + 1] - blockStarts[b]) >= minRows) << b;
  }
  // Most filtered steps have no dense block: the row kernel alone.
  if (dense == 0) {
    score(codes, codeSize, rows, count, out);
    return work;
  }
  // Dense blocks score whole; the rows of the others, contiguous runs of
  // `rows`, go to the row kernel together and come back in the same runs.
  alignas(32) float scores[ROWS];
  uint16_t sparse[BLOCK_ROWS];
  size_t sparseCount = 0;
  for (size_t b = 0; b < blockCount; b++) {
    size_t begin = blockStarts[b], end = blockStarts[b + 1];
    if ((dense >> b) & 1) {
      scoreBlocks(blocks + b * layout.bytes, ROWS, scores);
      for (size_t k = begin; k < end; k++) out[k] = scores[rows[k] % ROWS];
      work.blocks++;
      work.rows += end - begin;
    } else {
      std::memcpy(sparse + sparseCount, rows + begin, (end - begin) * sizeof(uint16_t));
      sparseCount += end - begin;
    }
  }
  if (sparseCount != 0) {
    alignas(64) float scored[BLOCK_ROWS];
    score(codes, codeSize, sparse, sparseCount, scored);
    for (size_t b = 0, at = 0; b < blockCount; b++) {
      size_t begin = blockStarts[b], end = blockStarts[b + 1];
      if ((dense >> b) & 1) continue;
      std::memcpy(out + begin, scored + at, (end - begin) * sizeof(float));
      at += end - begin;
    }
  }
  return work;
}

}  // namespace luxir
