// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "VectorKernels.h"

#include <bit>
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
struct LoadBF16 {
  static constexpr int32_t BYTES = 2;
  static Vec load(const char* p) {
    __m256i wide = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)p));
    return _mm256_castsi256_ps(_mm256_slli_epi32(wide, 16));
  }
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
struct LoadBF16 {
  static constexpr int32_t BYTES = 2;
  static Vec load(const char* p) {
    return vreinterpretq_f32_u32(vshll_n_u16(vreinterpret_u16_u8(vld1_u8((const uint8_t*)p)), 16));
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
struct LoadBF16 {
  static constexpr int32_t BYTES = 2;
  static Vec load(const char* p) {
    uint16_t h[W];
    std::memcpy(h, p, sizeof(h));
    Vec r;
    for (int32_t j = 0; j < W; j++) r.v[j] = std::bit_cast<float>((uint32_t)h[j] << 16);
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

// popcount(a ^ b) of the bytes past the SIMD body, a 64-bit word at a time.
inline int64_t hammingTail(const uint8_t* a, const uint8_t* b, int32_t i, int32_t n) {
  int64_t count = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t x;
    uint64_t y;
    std::memcpy(&x, a + i, 8);
    std::memcpy(&y, b + i, 8);
    count += std::popcount(x ^ y);
  }
  for (; i < n; i++) count += std::popcount((uint8_t)(a[i] ^ b[i]));
  return count;
}

#if defined(LUXIR_VKERNEL_AVX2)

// AVX2 has no vector popcount: count each nibble through a 16-entry shuffle
// table (Mula, Kurz and Lemire, "Faster Population Counts Using AVX2
// Instructions", 2018) and sum the byte counts with vpsadbw.
int64_t hammingBody(const uint8_t* a, const uint8_t* b, int32_t n, int32_t& i) {
  const __m256i table = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                         0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
  const __m256i low = _mm256_set1_epi8(0x0f);
  __m256i acc = _mm256_setzero_si256();
  for (; i + 32 <= n; i += 32) {
    __m256i x = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i)),
                                 _mm256_loadu_si256((const __m256i*)(b + i)));
    __m256i lo = _mm256_shuffle_epi8(table, _mm256_and_si256(x, low));
    __m256i hi = _mm256_shuffle_epi8(table, _mm256_and_si256(_mm256_srli_epi16(x, 4), low));
    acc = _mm256_add_epi64(acc, _mm256_sad_epu8(_mm256_add_epi8(lo, hi), _mm256_setzero_si256()));
  }
  return _mm256_extract_epi64(acc, 0) + _mm256_extract_epi64(acc, 1)
      + _mm256_extract_epi64(acc, 2) + _mm256_extract_epi64(acc, 3);
}

#elif defined(LUXIR_VKERNEL_NEON)

int64_t hammingBody(const uint8_t* a, const uint8_t* b, int32_t n, int32_t& i) {
  uint32x4_t acc = vdupq_n_u32(0);
  for (; i + 16 <= n; i += 16) {
    uint8x16_t counts = vcntq_u8(veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i)));
    acc = vpadalq_u16(acc, vpaddlq_u8(counts));
  }
  return (int64_t)vaddvq_u32(acc);
}

#else

// The 64-bit word loop of hammingTail is the portable body (POPCNT on
// x86-64-v2).
int64_t hammingBody(const uint8_t*, const uint8_t*, int32_t, int32_t&) {
  return 0;
}

#endif

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

float dotBF16(const float* q, const void* x, int32_t n) {
  return kernel<LoadBF16, false>(q, (const char*)x, n);
}

float l2sqBF16(const float* q, const void* x, int32_t n) {
  return kernel<LoadBF16, true>(q, (const char*)x, n);
}

int32_t hamming(const uint8_t* a, const uint8_t* b, int32_t n) {
  int32_t i = 0;
  int64_t count = hammingBody(a, b, n, i);
  return (int32_t)(count + hammingTail(a, b, i, n));
}

float dotU8(const float* q, const uint8_t* c, int32_t n) {
  return kernel<LoadU8, false>(q, (const char*)c, n);
}

const char* isa() { return ISA; }

} // namespace luxir::vkernel
