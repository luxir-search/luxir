// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>

namespace luxir::sq8 {

/// Entry points for one SIMD level. Each code decodes as
/// x[i] = offset + scale * code[i] (one FMA) inside the distance or dot
/// product. Scoring the decoded values avoids catastrophic cancellation for
/// queries with large dynamic range. Within a level the lane layout, tail and
/// reduction order depend only on n.
struct Kernels {
  // CPU level name ("avx2" or "avx512").
  const char* name;
  float (*l2sq)(const float* q, const uint8_t* codes, float offset, float scale, int32_t n);
  float (*dot)(const float* q, const uint8_t* codes, float offset, float scale, int32_t n);
  // Inner product and squared row norm in one pass.
  float (*dotNorm)(const float* q, const uint8_t* codes, float offset, float scale, int32_t n, float& normSq);
};

/// Levels available on this CPU, fastest last. Empty outside GCC x86-64 or
/// on CPUs without AVX2 and FMA. The 16-lane level also requires AVX-512F,
/// AVX-512BW and AVX-512VL.
std::span<const Kernels> supported();

/// Fastest supported level, or nullptr when the caller must decode the row.
const Kernels* best();

} // namespace luxir::sq8
