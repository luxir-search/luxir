// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace luxir {
namespace rabitq {

// Coefficients for one query and list centroid.
struct Factors {
  // Coefficients for the weighted query count, row popcount and constant term.
  float c1 = 0, c2 = 0, c34 = 0;
  float queryToCentroidSqr = 0, queryNormSqr = 0;
};

// Integer terms of the one-bit estimate, before applying the factors.
struct Counts {
  // Sum of quantized query values where the row bit is set.
  uint64_t dot = 0;
  uint64_t popcount = 0;
};

// Query and list state shared by all kernel levels.
struct State {
  const float* query;
  size_t dims;
  // Bytes of one row's binary code, excluding its float factors.
  size_t codeBytes;
  // Full SIMD chunks (the last may be masked) followed by 64-bit words.
  size_t chunks = 0, tailWords = 0;
  int32_t queryBits;
  bool innerProduct;
  float invSqrtDims;
  // Largest quantized query value.
  float maxCode;
  // FAISS's g_error: L2 distance from the query to the current centroid.
  float gError = 0;
  Factors factors;
  float* residual = nullptr;
  uint8_t* quantized = nullptr;
  // Planes grouped by SIMD chunk, then by tail word.
  uint8_t* planes = nullptr;
};

struct Kernels;

}  // namespace rabitq

// Replacement for faiss::RaBitQDistanceComputerQ::distance_to_code_1bit
// on non-centered 1-bit codes with 1 to 8 query bits, under L2 or inner product.
//
// Quantizes each residual by rounding (value - low) / delta half up, clamped
// to [0, 2^queryBits - 1]; constant residuals quantize to zero. Float estimates
// may vary with compiler contraction and kernel level.
//
// One instance serves one scan task. The constructor binds the kernel level
// and allocates its query layout; setList() re-quantizes into those buffers.
// GCC x86-64 builds select a level at run time. Scoring dispatches once per
// block of rows.
class RaBitQScorer {
  // The level fixes the plane layout for the lifetime of the scorer.
  const rabitq::Kernels* kernels;
  rabitq::State state;
  // One allocation holds the residual, quantized bytes and planes.
  std::unique_ptr<uint8_t[]> storage;

public:
  using Factors = rabitq::Factors;
  using Counts = rabitq::Counts;

  // BEST is the fastest supported level for the code size: the first level
  // for codes of at most two 64-bit words, the last otherwise.
  enum class Level { GENERIC, AVX2, AVX512, NEON, BEST };
  // Row offsets fit in 16 bits: the scan selects at most one block of rows.
  static constexpr size_t BLOCK_ROWS = 256;

  // Levels available on this CPU, widest last.
  static std::span<const Level> supported();
  RaBitQScorer(int32_t dims, int32_t queryBits, bool innerProduct, const float* query, Level level = Level::BEST);

  // Prepares for the codes of the list with this (rotated-space) centroid.
  void setList(const float* centroid);

  float queryToCentroid() const { return state.gError; }
  const Factors& listFactors() const { return state.factors; }
  // Quantized values, planes and the scoring kernel's integer counts; for tests.
  uint8_t quantizedValue(size_t index) const { return state.quantized[index]; }
  Counts counts(const uint8_t* code) const;
  uint8_t planeByte(int32_t plane, size_t index) const;

  // out[i] = estimated distance of row i, for `count` rows at `codes`.
  void score(const uint8_t* codes, size_t codeSize, size_t count, float* out) const;
  // out[i] = estimated distance of row `rows[i]`.
  void score(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
};

}  // namespace luxir
