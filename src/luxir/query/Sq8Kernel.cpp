// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Sq8Kernel.h"

#if defined(__AVX2__) && defined(__FMA__)

#include <immintrin.h>

#include <cmath>

namespace luxir::sq8 {
namespace {

enum class Op { L2, DOT, DOT_NORM };

constexpr int32_t W = 8;
// Independent accumulators per sum.  Each 8-lane step is two or three FMAs
// plus a widening load, so four chains already hide the FMA latency.
constexpr int32_t ACC = 4;

inline __m256 decode(const uint8_t* p, __m256 offset, __m256 scale) {
  __m256i wide = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)p));
  return _mm256_fmadd_ps(scale, _mm256_cvtepi32_ps(wide), offset);
}

inline float hsum(const __m256* a) {
  __m256 v = _mm256_add_ps(_mm256_add_ps(a[0], a[1]), _mm256_add_ps(a[2], a[3]));
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_movehdup_ps(s));
  return _mm_cvtss_f32(s);
}

template <Op OP>
inline void step(__m256& sum, __m256& norm, const float* q, const uint8_t* c, __m256 offset, __m256 scale) {
  __m256 x = decode(c, offset, scale);
  __m256 qv = _mm256_loadu_ps(q);
  if constexpr (OP == Op::L2) {
    __m256 d = _mm256_sub_ps(qv, x);
    sum = _mm256_fmadd_ps(d, d, sum);
  } else {
    sum = _mm256_fmadd_ps(qv, x, sum);
    if constexpr (OP == Op::DOT_NORM) norm = _mm256_fmadd_ps(x, x, norm);
  }
}

// ACC accumulators over blocks of ACC * W elements, single steps into the
// first, a fixed tree reduction, then a scalar tail with the same FMA decode.
template <Op OP>
float kernel(const float* q, const uint8_t* c, float offset, float scale, int32_t n, float* normSq) {
  __m256 vo = _mm256_set1_ps(offset);
  __m256 vs = _mm256_set1_ps(scale);
  __m256 sum[ACC];
  __m256 norm[ACC];
  for (int32_t j = 0; j < ACC; j++) sum[j] = norm[j] = _mm256_setzero_ps();
  int32_t i = 0;
  for (; i + ACC * W <= n; i += ACC * W) {
    for (int32_t j = 0; j < ACC; j++) step<OP>(sum[j], norm[j], q + i + j * W, c + i + j * W, vo, vs);
  }
  for (; i + W <= n; i += W) step<OP>(sum[0], norm[0], q + i, c + i, vo, vs);
  float s = hsum(sum);
  float nn = OP == Op::DOT_NORM ? hsum(norm) : 0.0f;
  for (; i < n; i++) {
    float x = std::fma(scale, (float)c[i], offset);
    if constexpr (OP == Op::L2) {
      float d = q[i] - x;
      s = std::fma(d, d, s);
    } else {
      s = std::fma(q[i], x, s);
      if constexpr (OP == Op::DOT_NORM) nn = std::fma(x, x, nn);
    }
  }
  if constexpr (OP == Op::DOT_NORM) *normSq = nn;
  return s;
}

} // namespace

float l2sq(const float* q, const uint8_t* codes, float offset, float scale, int32_t n) {
  return kernel<Op::L2>(q, codes, offset, scale, n, nullptr);
}

float dot(const float* q, const uint8_t* codes, float offset, float scale, int32_t n) {
  return kernel<Op::DOT>(q, codes, offset, scale, n, nullptr);
}

float dotNorm(const float* q, const uint8_t* codes, float offset, float scale, int32_t n, float& normSq) {
  return kernel<Op::DOT_NORM>(q, codes, offset, scale, n, &normSq);
}

} // namespace luxir::sq8

#endif
