// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
//
// Built with -ffp-contract=off: FAISS compiles its distance computer without
// FMA on x86-64 whatever the target of its dependents, while GCC contracts it
// on AArch64. Fused operations here are therefore explicit (fusedAdd below), so
// the estimate keeps the rounding of FAISS's at every optimization level.
#include "RaBitQScorer.h"

#include "luxir/util/luxir_util.h"
#include <faiss/utils/distances.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE4_1__)
#include <smmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace luxir {
namespace {

#if defined(__AVX2__)
constexpr size_t CHUNK_BYTES = 32;
#elif defined(__ARM_NEON)
constexpr size_t CHUNK_BYTES = 16;
#else
constexpr size_t CHUNK_BYTES = 0;  // Hardware popcount over 64-bit words.
#endif

constexpr size_t PREFETCH_ROWS = 6;

// a * b + c, fused where FAISS's own build fuses it.
inline LUXIR_INLINE float fusedAdd(float a, float b, float c) {
#if defined(__aarch64__)
  return __builtin_fmaf(a, b, c);
#else
  return a * b + c;
#endif
}

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

// std::clamp(std::round(x), 0, maxCode) for x >= 0 or NaN. NaN (a list whose
// residual is constant has delta 0, so 0 * inf) gives 0, as FAISS's float to
// uint8_t cast does in practice; c1 and delta are then 0 and hide the value. x
// minus its truncation is exact, so the half test has no rounding of its own.
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

#if defined(__AVX2__)
struct ChunkOps {
  __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
  __m256i low = _mm256_set1_epi8(0x0f);

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
  template <int QB>
  LUXIR_INLINE void score(const uint8_t*, const uint8_t*, size_t, uint64_t& dot, uint64_t& sum) const {
    dot = 0;
    sum = 0;
  }
};
#endif

}  // namespace

RaBitQScorer::RaBitQScorer(int32_t dims, int32_t queryBits, bool innerProduct, const float* query)
    : query(query), dims((size_t)dims), codeBytes(((size_t)dims + 7) / 8),
      chunks(CHUNK_BYTES == 0 ? 0 : codeBytes / std::max(CHUNK_BYTES, (size_t)1)),
      tailWords((codeBytes - chunks * CHUNK_BYTES + 7) / 8), queryBits(queryBits), innerProduct(innerProduct),
      invSqrtDims(1.0f / std::sqrt((float)dims)), maxCode((float)((1 << queryBits) - 1)) {
  assert(dims > 0 && queryBits >= 1 && queryBits <= 8);
  constexpr size_t ALIGN = 64;
  size_t residualBytes = (this->dims * sizeof(float) + ALIGN - 1) & ~(ALIGN - 1);
  // Whole 64-bit groups, so packPlanes can load past the last dimension.
  size_t quantizedBytes = (this->dims + 7 + ALIGN - 1) & ~(ALIGN - 1);
  size_t planeBytes = ((chunks * CHUNK_BYTES + tailWords * 8) * (size_t)queryBits + ALIGN - 1) & ~(ALIGN - 1);
  storage = std::make_unique_for_overwrite<uint8_t[]>(residualBytes + quantizedBytes + planeBytes + ALIGN);
  uint8_t* base = storage.get() + (ALIGN - (uintptr_t)storage.get() % ALIGN) % ALIGN;
  residual = (float*)base;
  quantized = base + residualBytes;
  planes = quantized + quantizedBytes;
  std::memset(quantized, 0, quantizedBytes + planeBytes);
  if (innerProduct) factors.queryNormSqr = faiss::fvec_norm_L2sqr(query, this->dims);
}

// FAISS packs one bit at a time; this takes 8 (or, on AVX2, 32) dimensions at once.
void RaBitQScorer::packPlanes() {
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
  for (size_t c = 0; c < chunks; c++, left -= CHUNK_BYTES * 8) {
#if defined(__AVX2__)
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
  size_t qb = (size_t)queryBits;
  size_t chunkBytes = chunks * CHUNK_BYTES;
  if constexpr (CHUNK_BYTES != 0) {
    if (index < chunkBytes)
      return planes[(index / CHUNK_BYTES) * qb * CHUNK_BYTES + (size_t)plane * CHUNK_BYTES + index % CHUNK_BYTES];
  }
  index -= chunkBytes;
  return planes[chunkBytes * qb + (index / 8) * qb * 8 + (size_t)plane * 8 + index % 8];
}

void RaBitQScorer::setList(const float* centroid) {
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
  packPlanes();
  factors.c1 = 2 * delta * invSqrtDims;
  factors.c2 = 2 * low * invSqrtDims;
  factors.c34 = invSqrtDims * fusedAdd(delta, (float)sum, (float)dims * low);
}

template <int QB, bool IP, bool Selected>
LUXIR_NOINLINE void RaBitQScorer::scoreBlock(const uint8_t* codes, size_t codeSize, const uint16_t* rows,
                                             size_t count, float* LUXIR_RESTRICT out) const {
  const ChunkOps ops;
  const size_t chunkCount = chunks, words = tailWords, binary = codeBytes;
  const size_t wordsAt = chunkCount * CHUNK_BYTES;
  const uint8_t* wordPlanes = planes + wordsAt * QB;
  // Only the last word can run past the sign bits, into the 8 factor bytes that
  // follow them in every row, so it stays inside the row; plane words are 0 there.
  const uint64_t lastMask = binary % 8 == 0 ? ~(uint64_t)0 : ((uint64_t)1 << (binary % 8 * 8)) - 1;
  const float c1 = factors.c1, c2 = factors.c2, c34 = factors.c34;
  const float queryToCentroid = factors.queryToCentroidSqr, queryNorm = factors.queryNormSqr;
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
    uint64_t dot = 0, sum = 0;
    if constexpr (CHUNK_BYTES != 0) {
      if (chunkCount != 0) ops.template score<QB>(code, planes, chunkCount, dot, sum);
    }
    const uint8_t* plane = wordPlanes;
    for (size_t w = 0; w < words; w++, plane += QB * sizeof(uint64_t)) {
      uint64_t row = load64(code + wordsAt + w * sizeof(uint64_t));
      for (int p = 0; p < QB; p++)
        dot += (uint64_t)std::popcount(row & load64(plane + (size_t)p * sizeof(uint64_t))) << p;
      sum += (uint64_t)std::popcount(w + 1 == words ? row & lastMask : row);
    }
    float orMinusC, dpMultiplier;
    std::memcpy(&orMinusC, code + binary, sizeof(float));
    std::memcpy(&dpMultiplier, code + binary + sizeof(float), sizeof(float));
    // In FAISS's order; the leading 0 + c1 * dot changes no value, c1 being >= 0.
    float finalDot = fusedAdd(c2, (float)(int64_t)sum, c1 * (float)(int64_t)dot) - c34;
    float preDist = fusedAdd(-(2 * dpMultiplier), finalDot, orMinusC + queryToCentroid);
    out[i] = IP ? -0.5f * (preDist - queryNorm) : preDist;
  }
}

template <bool Selected>
void RaBitQScorer::dispatch(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const {
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

}  // namespace luxir
