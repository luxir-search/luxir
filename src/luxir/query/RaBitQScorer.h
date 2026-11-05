// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace luxir {

// Inlined replacement for faiss::RaBitQDistanceComputerQ::distance_to_code_1bit
// on non-centered 1-bit codes with 1 to 8 query bits, under L2 or inner product.
//
// Results match the FAISS estimate exactly: the query-to-centroid norms come
// from the same FAISS distance functions, the quantized query and its bit
// planes are the same integers, and the float expressions run in FAISS's order
// (see RaBitQScorer.cpp for how contraction into FMA is kept equal).
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
  // Row offsets fit in 16 bits: the scan selects at most one block of rows.
  static constexpr size_t BLOCK_ROWS = 256;

private:
  const float* query;
  size_t dims;
  size_t codeBytes;
  size_t chunks;
  size_t tailWords;
  int32_t queryBits;
  bool innerProduct;
  float invSqrtDims;
  float maxCode;
  float gError = 0;
  Factors factors;
  // One allocation: residual floats, quantized bytes, then bit planes. Planes
  // group by position, not by plane: first `chunks` SIMD chunks of `queryBits`
  // planes each, then `tailWords` 64-bit words of `queryBits` planes each.
  std::unique_ptr<uint8_t[]> storage;
  float* residual;
  uint8_t* quantized;
  uint8_t* planes;

  template <int QB, bool IP, bool Selected>
  void scoreBlock(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
  template <bool Selected>
  void dispatch(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
  void packPlanes();

public:
  RaBitQScorer(int32_t dims, int32_t queryBits, bool innerProduct, const float* query);

  // Prepares for the codes of the list with this (rotated-space) centroid.
  void setList(const float* centroid);

  // FAISS's g_error: the L2 distance from the query to the current centroid.
  float queryToCentroid() const { return gError; }
  const Factors& listFactors() const { return factors; }
  // Byte `index` of bit plane `plane`, as FAISS lays it out; for tests.
  uint8_t planeByte(int32_t plane, size_t index) const;

  // out[i] = estimated distance of row i, for `count` rows at `codes`.
  void score(const uint8_t* codes, size_t codeSize, size_t count, float* out) const;
  // out[i] = estimated distance of row `rows[i]`.
  void score(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
};

}  // namespace luxir
