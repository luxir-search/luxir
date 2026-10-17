// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "VectorKernels.h"

#include <cstring>

#if defined(__AVX2__) && defined(__FMA__) && defined(__F16C__)
#define LUXIR_VKERNEL_AVX2 1
#include <immintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#define LUXIR_VKERNEL_NEON 1
#include <arm_neon.h>
#else
#include <stdfloat>
#endif

namespace luxir::vkernel {
namespace {

// Every path shares one shape: ACC vector accumulators over the main loop
// (ACC * W elements per iteration), single-vector steps into the first one, a
// zero-padded tail step into the second, then a fixed pairwise tree over the
// accumulators and a fixed horizontal reduction.  Zero padding contributes
// exactly 0 to both dot and squared-distance sums.  Row pointers are byte
// addresses: stored rows carry no alignment guarantee.
//
// Columns larger than the cache make the scan memory-bound, so the loop is
// shaped for memory-level parallelism: eight independent accumulators (a
// wide iteration keeps more cache-line misses in flight) and one software
// prefetch PREFETCH_BYTES ahead per iteration, which runs into the next row
// of a contiguous scan.  Measured on a 150 MB f32 column, this moved a single
// thread from ~29 to ~37 GB/s, matching the FAISS kernel it replaces.

#if defined(LUXIR_VKERNEL_AVX2)

constexpr int32_t W = 8;
using Vec = __m256;

struct LoadF32 {
  static constexpr int32_t BYTES = 4;
  static Vec load(const char* p) { return _mm256_loadu_ps((const float*)p); }
};
struct LoadF16 {
  static constexpr int32_t BYTES = 2;
  static Vec load(const char* p) { return _mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)p)); }
};
struct LoadU8 {
  static constexpr int32_t BYTES = 1;
  static Vec load(const char* p) {
    return _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)p)));
  }
};

inline Vec zero() { return _mm256_setzero_ps(); }
inline Vec loadQ(const float* p) { return _mm256_loadu_ps(p); }
inline Vec add(Vec a, Vec b) { return _mm256_add_ps(a, b); }
template <bool L2>
inline Vec step(Vec acc, Vec q, Vec x) {
  if constexpr (L2) {
    Vec d = _mm256_sub_ps(q, x);
    return _mm256_fmadd_ps(d, d, acc);
  } else {
    return _mm256_fmadd_ps(q, x, acc);
  }
}
inline float hsum(Vec v) {
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_movehdup_ps(s));
  return _mm_cvtss_f32(s);
}
constexpr const char* ISA = "avx2-f16c";

#elif defined(LUXIR_VKERNEL_NEON)

constexpr int32_t W = 4;
using Vec = float32x4_t;

struct LoadF32 {
  static constexpr int32_t BYTES = 4;
  static Vec load(const char* p) { return vreinterpretq_f32_u8(vld1q_u8((const uint8_t*)p)); }
};
struct LoadF16 {
  static constexpr int32_t BYTES = 2;
  static Vec load(const char* p) {
    return vcvt_f32_f16(vreinterpret_f16_u8(vld1_u8((const uint8_t*)p)));
  }
};
struct LoadU8 {
  static constexpr int32_t BYTES = 1;
  static Vec load(const char* p) {
    uint32_t bits;
    std::memcpy(&bits, p, sizeof(bits));
    uint16x8_t wide = vmovl_u8(vcreate_u8((uint64_t)bits));
    return vcvtq_f32_u32(vmovl_u16(vget_low_u16(wide)));
  }
};

inline Vec zero() { return vdupq_n_f32(0.0f); }
inline Vec loadQ(const float* p) { return vld1q_f32(p); }
inline Vec add(Vec a, Vec b) { return vaddq_f32(a, b); }
template <bool L2>
inline Vec step(Vec acc, Vec q, Vec x) {
  if constexpr (L2) {
    Vec d = vsubq_f32(q, x);
    return vfmaq_f32(acc, d, d);
  } else {
    return vfmaq_f32(acc, q, x);
  }
}
inline float hsum(Vec v) { return vaddvq_f32(v); }
constexpr const char* ISA = "neon";

#else

// Portable path: a fixed-width lane array the compiler vectorizes for the
// baseline target (SSE on x86-64-v2).
constexpr int32_t W = 8;
struct Vec {
  float v[W];
};

struct LoadF32 {
  static constexpr int32_t BYTES = 4;
  static Vec load(const char* p) {
    Vec r;
    std::memcpy(r.v, p, sizeof(r.v));
    return r;
  }
};
struct LoadF16 {
  static constexpr int32_t BYTES = 2;
  static Vec load(const char* p) {
    std::float16_t h[W];
    std::memcpy(h, p, sizeof(h));
    Vec r;
    for (int32_t j = 0; j < W; j++) r.v[j] = (float)h[j];
    return r;
  }
};
struct LoadU8 {
  static constexpr int32_t BYTES = 1;
  static Vec load(const char* p) {
    Vec r;
    for (int32_t j = 0; j < W; j++) r.v[j] = (float)(uint8_t)p[j];
    return r;
  }
};

inline Vec zero() { return Vec{}; }
inline Vec loadQ(const float* p) {
  Vec r;
  std::memcpy(r.v, p, sizeof(r.v));
  return r;
}
inline Vec add(Vec a, Vec b) {
  for (int32_t j = 0; j < W; j++) a.v[j] += b.v[j];
  return a;
}
template <bool L2>
inline Vec step(Vec acc, Vec q, Vec x) {
  for (int32_t j = 0; j < W; j++) {
    float d = L2 ? q.v[j] - x.v[j] : q.v[j];
    float e = L2 ? d : x.v[j];
    acc.v[j] += d * e;
  }
  return acc;
}
inline float hsum(Vec v) {
  return ((v.v[0] + v.v[1]) + (v.v[2] + v.v[3])) + ((v.v[4] + v.v[5]) + (v.v[6] + v.v[7]));
}
constexpr const char* ISA = "scalar";

#endif

constexpr int32_t ACC = 8;
constexpr int32_t PREFETCH_BYTES = 2048;

template <class Load, bool L2>
float kernel(const float* q, const char* x, int32_t n) {
  Vec a[ACC];
  for (int32_t j = 0; j < ACC; j++) a[j] = zero();
  int32_t i = 0;
  for (; i + ACC * W <= n; i += ACC * W) {
    __builtin_prefetch(x + i * Load::BYTES + PREFETCH_BYTES);
    for (int32_t j = 0; j < ACC; j++) {
      a[j] = step<L2>(a[j], loadQ(q + i + j * W), Load::load(x + (i + j * W) * Load::BYTES));
    }
  }
  for (; i + W <= n; i += W) {
    a[0] = step<L2>(a[0], loadQ(q + i), Load::load(x + i * Load::BYTES));
  }
  if (i < n) {
    float qt[W] = {};
    char xt[W * Load::BYTES] = {};
    std::memcpy(qt, q + i, (size_t)(n - i) * sizeof(float));
    std::memcpy(xt, x + i * Load::BYTES, (size_t)(n - i) * Load::BYTES);
    a[1] = step<L2>(a[1], loadQ(qt), Load::load(xt));
  }
  Vec s01 = add(a[0], a[1]);
  Vec s23 = add(a[2], a[3]);
  Vec s45 = add(a[4], a[5]);
  Vec s67 = add(a[6], a[7]);
  return hsum(add(add(s01, s23), add(s45, s67)));
}

} // namespace

float dotF32(const float* q, const void* x, int32_t n) {
  return kernel<LoadF32, false>(q, (const char*)x, n);
}

float l2sqF32(const float* q, const void* x, int32_t n) {
  return kernel<LoadF32, true>(q, (const char*)x, n);
}

float dotF16(const float* q, const void* x, int32_t n) {
  return kernel<LoadF16, false>(q, (const char*)x, n);
}

float l2sqF16(const float* q, const void* x, int32_t n) {
  return kernel<LoadF16, true>(q, (const char*)x, n);
}

float dotU8(const float* q, const uint8_t* c, int32_t n) {
  return kernel<LoadU8, false>(q, (const char*)c, n);
}

const char* isa() { return ISA; }

} // namespace luxir::vkernel
