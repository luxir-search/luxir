// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace luxir {

// Inlined replacement for faiss::RaBitQDistanceComputerQ::distance_to_code_1bit
// on non-centered 1-bit codes with 1 to 8 query bits, under L2 or inner product.
// With 2 to 9 code bits it also replaces distance_to_code_full on the split
// rows of RaBitQLayout: the extended estimate is the integer inner product of
// all code bit planes with an 8-bit quantization of the residual, whatever the
// 1-bit pass's query bits (9-bit codes multiply their extra byte instead).
// FAISS multiplies float residuals, so the extended estimates agree with it
// within 8-bit query-quantization error.
//
// Quantizes each residual by rounding (value - low) / delta half up, clamped
// to [0, 2^queryBits - 1]; constant residuals quantize to zero. Float estimates
// may vary with compiler contraction and architecture.
//
// One instance serves one scan task. The constructor allocates the query-level
// buffers; setList() re-quantizes the residual into them for each list without
// allocating. Codes are read in blocks, so the per-code path has no virtual
// call and no indirect branch.
class RaBitQScorer {
public:
  struct Factors {
    float c1 = 0, c2 = 0, c34 = 0, queryToCentroidSqr = 0, queryNormSqr = 0;
  };
  struct Counts { uint64_t dot = 0, popcount = 0; };
  // The extended stage's 8-bit query: values are low + delta * code.
  struct ExtendedFactors {
    float delta = 0, low = 0, base = 0;  // base: ||q - c||^2 for L2, <q, c> for IP
    uint64_t sum = 0;
  };
  static constexpr int32_t EXTENDED_QUERY_BITS = 8;
  // Row offsets fit in 16 bits: the scan selects at most one block of rows.
  static constexpr size_t BLOCK_ROWS = 256;

private:
  const float* query;
  size_t dims;
  size_t codeBytes;
  size_t chunks;
  size_t tailWords;
  int32_t queryBits;
  int32_t codeBits;
  bool innerProduct;
  float invSqrtDims;
  float maxCode;
  float gError = 0;
  Factors factors;
  ExtendedFactors extendedFactors;
  // Most lists pass no row through the gate: quantize for them on demand.
  bool extendedReady = true;
  float extendedHigh = 0;
  // One allocation: residual floats, quantized bytes, then bit planes. Planes
  // group by position, not by plane: first `chunks` SIMD chunks of `queryBits`
  // planes each, then `tailWords` 64-bit words of `queryBits` planes each. With
  // masked loads, the final chunk can be partial and zero padded, with no tail.
  // Extended codes add the 8-bit quantization and its planes, or share the
  // 1-bit pass's when it has 8 query bits.
  std::unique_ptr<uint8_t[]> storage;
  float* residual;
  uint8_t* quantized;
  uint8_t* planes;
  uint8_t* extendedQuantized;
  uint8_t* extendedPlanes;

  template <int QB, class Ops> Counts countCode(const uint8_t* code, const uint8_t* queryPlanes, const Ops& ops) const;
  template <int QB, bool IP, bool Selected>
  void scoreBlock(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
  template <bool Selected>
  void dispatch(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
  void packPlanes(const uint8_t* in, uint8_t* out, size_t qb) const;

public:
  // codeBits > 1 also prepares extended(); codes then follow RaBitQLayout.
  RaBitQScorer(int32_t dims, int32_t queryBits, bool innerProduct, const float* query, int32_t codeBits = 1);

  // Prepares for the codes of the list with this (rotated-space) centroid.
  void setList(const float* centroid);

  // FAISS's g_error: the L2 distance from the query to the current centroid.
  float queryToCentroid() const { return gError; }
  const Factors& listFactors() const { return factors; }
  // The extended stage's own quantization, which extended() runs on its first
  // row in a list. Factors and values are those of the last prepared list.
  void prepareExtended();
  const ExtendedFactors& listExtendedFactors() const { return extendedFactors; }
  // Quantized values, planes and the scoring kernel's integer counts; for tests.
  uint8_t quantizedValue(size_t index) const { return quantized[index]; }
  uint8_t extendedQuantizedValue(size_t index) const { return extendedQuantized[index]; }
  Counts counts(const uint8_t* code) const;
  uint8_t planeByte(int32_t plane, size_t index) const;
  // Over all code bits: dot = sum of code value * 8-bit query value, popcount =
  // sum of code values. `sign` and `extra` are one row's RaBitQLayout regions.
  Counts extendedCounts(const uint8_t* sign, const uint8_t* extra);

  // FAISS's full multi-bit estimate of one row, without its L2 clamp at zero.
  float extended(const uint8_t* sign, const uint8_t* extra);

  // out[i] = estimated distance of row i, for `count` rows at `codes`.
  void score(const uint8_t* codes, size_t codeSize, size_t count, float* out) const;
  // out[i] = estimated distance of row `rows[i]`.
  void score(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
};

}  // namespace luxir
