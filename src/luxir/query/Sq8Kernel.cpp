// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Sq8Kernel.h"

// The target regions below are GCC's; other compilers get no kernels.
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
#define LUXIR_SQ8_X86
#include <cstring>
#include <immintrin.h>

#include "luxir/util/luxir_util.h"
#endif

namespace luxir::sq8 {

#ifdef LUXIR_SQ8_X86
namespace {

enum class Op { L2, DOT, DOT_NORM };

// Independent accumulators per sum. Four chains hide the FMA latency.
constexpr int32_t ACC = 4;

// Keep the shared definitions inside each target region. All its functions
// have internal linkage through this anonymous namespace.
#pragma GCC push_options
#pragma GCC target("avx2,fma")
namespace avx2 {

using Vec = __m256;
using Mask = __m256i;
constexpr int32_t W = 8;

inline LUXIR_INLINE Vec broadcast(float x) { return _mm256_set1_ps(x); }
inline LUXIR_INLINE Vec add(Vec a, Vec b) { return _mm256_add_ps(a, b); }
inline LUXIR_INLINE Vec sub(Vec a, Vec b) { return _mm256_sub_ps(a, b); }
inline LUXIR_INLINE Vec fmadd(Vec a, Vec b, Vec c) { return _mm256_fmadd_ps(a, b, c); }
inline LUXIR_INLINE Vec loadQuery(const float* q) { return _mm256_loadu_ps(q); }

inline LUXIR_INLINE Vec widenCodes(__m128i codes) {
  return _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(codes));
}

inline LUXIR_INLINE Vec loadCodes(const uint8_t* c) {
  return widenCodes(_mm_loadl_epi64((const __m128i*)c));
}

inline LUXIR_INLINE Mask tailMask(int32_t n) {
  return _mm256_cmpgt_epi32(_mm256_set1_epi32(n), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
}

inline LUXIR_INLINE Vec loadTailQuery(const float* q, Mask mask) {
  return _mm256_maskload_ps(q, mask);
}

// Copy from both ends using fixed-size words. The reads can overlap and the
// packed result stays zero above n bytes. Fixed sizes let GCC inline the copy.
template <typename Word>
inline LUXIR_INLINE uint64_t packTail(const uint8_t* c, int32_t n) {
  constexpr int32_t bytes = sizeof(Word);
  Word first, last;
  std::memcpy(&first, c, bytes);
  std::memcpy(&last, c + n - bytes, bytes);
  return (uint64_t)first | ((uint64_t)last << (8 * (n - bytes)));
}

inline LUXIR_INLINE Vec loadTailCodes(const uint8_t* c, int32_t n) {
  uint64_t codes = c[0];
  if (n >= 4) codes = packTail<uint32_t>(c, n);
  else if (n >= 2) codes = packTail<uint16_t>(c, n);
  return widenCodes(_mm_cvtsi64_si128((int64_t)codes));
}

inline LUXIR_INLINE Vec tailFmadd(Vec a, Vec b, Vec c, Mask mask) {
  return _mm256_and_ps(fmadd(a, b, c), _mm256_castsi256_ps(mask));
}

inline LUXIR_INLINE float horizontalSum(Vec v) {
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_movehdup_ps(s));
  return _mm_cvtss_f32(s);
}

#include "Sq8KernelBody.inl"

} // namespace avx2
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC target("avx2,fma,avx512f,avx512bw,avx512vl")
namespace avx512 {

using Vec = __m512;
using Mask = __mmask16;
constexpr int32_t W = 16;

inline LUXIR_INLINE Vec broadcast(float x) { return _mm512_set1_ps(x); }
inline LUXIR_INLINE Vec add(Vec a, Vec b) { return _mm512_add_ps(a, b); }
inline LUXIR_INLINE Vec sub(Vec a, Vec b) { return _mm512_sub_ps(a, b); }
inline LUXIR_INLINE Vec fmadd(Vec a, Vec b, Vec c) { return _mm512_fmadd_ps(a, b, c); }
inline LUXIR_INLINE Vec loadQuery(const float* q) { return _mm512_loadu_ps(q); }

inline LUXIR_INLINE Vec widenCodes(__m128i codes) {
  return _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(codes));
}

inline LUXIR_INLINE Vec loadCodes(const uint8_t* c) {
  return widenCodes(_mm_loadu_si128((const __m128i*)c));
}

inline LUXIR_INLINE Mask tailMask(int32_t n) {
  return (Mask)((1u << n) - 1);
}

inline LUXIR_INLINE Vec loadTailQuery(const float* q, Mask mask) {
  return _mm512_maskz_loadu_ps(mask, q);
}

// The masked byte load requires AVX-512BW and AVX-512VL.
inline LUXIR_INLINE Vec loadTailCodes(const uint8_t* c, int32_t n) {
  return widenCodes(_mm_maskz_loadu_epi8(tailMask(n), c));
}

inline LUXIR_INLINE Vec tailFmadd(Vec a, Vec b, Vec c, Mask mask) {
  return _mm512_maskz_fmadd_ps(mask, a, b, c);
}

inline LUXIR_INLINE float horizontalSum(Vec v) { return _mm512_reduce_add_ps(v); }

#include "Sq8KernelBody.inl"

} // namespace avx512
#pragma GCC pop_options

constexpr Kernels levels[] = {
  {"avx2", avx2::score<Op::L2>, avx2::score<Op::DOT>, avx2::dotNorm},
  {"avx512", avx512::score<Op::L2>, avx512::score<Op::DOT>, avx512::dotNorm},
};

} // namespace

std::span<const Kernels> supported() {
  static const auto available = [] {
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma")) return std::span<const Kernels>();
    bool wide = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512vl");
    return std::span<const Kernels>(levels, wide ? 2 : 1);
  }();
  return available;
}

#else

std::span<const Kernels> supported() {
  return {};
}

#endif

const Kernels* best() {
  auto available = supported();
  return available.empty() ? nullptr : &available.back();
}

} // namespace luxir::sq8
