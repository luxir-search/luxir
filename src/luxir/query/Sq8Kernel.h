// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace luxir::sq8 {

/// SIMD kernels that score a float query against one sq8 row in registers:
/// each code widens to x[i] = offset + scale * code[i] (one FMA) and feeds the
/// distance or dot product directly, with no decoded copy of the row.  The
/// decoded form is used rather than dot(q, codes) * scale + offset * sum(q),
/// which cancels catastrophically for a query with a large dynamic range.
///
/// The instruction set is the build's CPU target: the kernels exist when the
/// target has AVX2 and FMA (x86-64-v3 and up).  Elsewhere KERNEL is false and
/// VectorScorer decodes the row and calls the FAISS float32 kernels, which
/// dispatch at run time and beat a portable lane loop.
///
/// Lane layout, tail and reduction order depend only on n, and the kernels are
/// compiled once (Sq8Kernel.cpp), so a row scores bit-identically wherever it
/// is scored.
#if defined(__AVX2__) && defined(__FMA__)
inline constexpr bool KERNEL = true;
#else
inline constexpr bool KERNEL = false;
#endif

/// Squared L2 distance between q and the decoded row.
float l2sq(const float* q, const uint8_t* codes, float offset, float scale, int32_t n);
/// Inner product of q and the decoded row.
float dot(const float* q, const uint8_t* codes, float offset, float scale, int32_t n);
/// Inner product of q and the decoded row, and its squared norm in normSq,
/// in one pass over the codes.
float dotNorm(const float* q, const uint8_t* codes, float offset, float scale, int32_t n, float& normSq);

} // namespace luxir::sq8
