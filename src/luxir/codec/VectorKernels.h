// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace luxir::vkernel {

/// Similarity kernels of one float32 query against one stored row, per
/// column encoding.  The stored row may start at any byte address.
///
///   F32:  dot(q, x), |q - x|^2 over float32 x.
///   F16:  dot(q, x), |q - x|^2 over IEEE binary16 x, widened in registers
///         (no conversion pass to a buffer).
///   U8:   dot(q, c) over uint8 codes c widened to float in registers (the
///         int8 encoding's offset / scale are applied by the caller).
///
/// The instruction set is chosen at compile time from the target flags the
/// project builds with (LUXIR_CPU_TARGET): AVX2 + FMA + F16C on x86-64-v3 and
/// up, NEON on AArch64, and a portable lane-array loop otherwise.
///
/// Determinism: a kernel's result depends only on (query, row, n).  The lane
/// layout, accumulator count, tail handling and final reduction order are
/// fixed per n, so a row scores bit-identically wherever it is scored (flat
/// scan or rescore, any thread, any batch position).  The kernels live in one
/// translation unit so floating-point contraction cannot differ per call site.
float dotF32(const float* q, const void* x, int32_t n);
float l2sqF32(const float* q, const void* x, int32_t n);
float dotF16(const float* q, const void* x, int32_t n);
float l2sqF16(const float* q, const void* x, int32_t n);
float dotU8(const float* q, const uint8_t* c, int32_t n);

/// Name of the compiled kernel path ("avx2-f16c", "neon", "scalar").
const char* isa();

} // namespace luxir::vkernel
