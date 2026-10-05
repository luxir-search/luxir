// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#if defined(__aarch64__)
#include "fastpfor_neon.h"

// Luxir's four-lane delta transpose also needs these native NEON interleaves.
inline __m128i _mm_unpacklo_epi32(__m128i a, __m128i b) {
  return vreinterpretq_s64_s32(vzip1q_s32(
      vreinterpretq_s32_s64(a), vreinterpretq_s32_s64(b)));
}
inline __m128i _mm_unpackhi_epi32(__m128i a, __m128i b) {
  return vreinterpretq_s64_s32(vzip2q_s32(
      vreinterpretq_s32_s64(a), vreinterpretq_s32_s64(b)));
}
inline __m128i _mm_unpacklo_epi64(__m128i a, __m128i b) {
  return vzip1q_s64(a, b);
}
inline __m128i _mm_unpackhi_epi64(__m128i a, __m128i b) {
  return vzip2q_s64(a, b);
}
#else
#include <immintrin.h>
#endif
