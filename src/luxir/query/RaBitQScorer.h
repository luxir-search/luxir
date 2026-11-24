// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace luxir {

// 1-bit RaBitQ codes of one list in blocks of 32 rows, for the nibble lookup
// kernel. A row code is the FAISS layout: (dims + 7) / 8 sign bytes, dimension i
// in bit i % 8 of byte i / 8, then or_minus_c and dp_multiplier. A block holds,
// for each code byte p (dimensions 8p to 8p + 7, two 4-dimension groups):
//   32 bytes: [group 2p | group 2p + 1], 16 bytes each. Byte j of a half holds
//             the group's nibble of row j (low) and of row j + 16 (high).
// then or_minus_c[32], dp_multiplier[32] (float) and the code popcount[32]
// (uint16). Rows past the end of a list are zero.
struct RaBitQBlockLayout {
  static constexpr size_t ROWS = 32;
  // The popcount is a uint16 and counts every bit of the code bytes, padding
  // included, as the row kernel does: whole code bytes keep it below 65536.
  static constexpr int32_t MAX_DIMS = 65528;
  static_assert(MAX_DIMS % 8 == 0 && MAX_DIMS < 65536);

  size_t dims, codeBytes, nibbleBytes, bytes;

  explicit RaBitQBlockLayout(size_t dims)
      : dims(dims), codeBytes((dims + 7) / 8), nibbleBytes(codeBytes * ROWS),
        bytes(nibbleBytes + ROWS * (2 * sizeof(float) + sizeof(uint16_t))) {}

  static bool supports(int32_t dims) { return dims > 0 && dims <= MAX_DIMS; }
  static size_t blocks(size_t rows) { return (rows + ROWS - 1) / ROWS; }
  size_t orMinusCOffset() const { return nibbleBytes; }
  size_t dpMultiplierOffset() const { return nibbleBytes + ROWS * sizeof(float); }
  size_t popcountOffset() const { return nibbleBytes + 2 * ROWS * sizeof(float); }

  // Packs `count` (at most ROWS) row codes, `codeSize` bytes apart, into `out`.
  void pack(const uint8_t* codes, size_t codeSize, size_t count, uint8_t* out) const;
  // Sign bit of dimension `dim` of row `row`; for tests.
  bool bit(const uint8_t* block, size_t row, size_t dim) const;
};

// Inlined replacement for faiss::RaBitQDistanceComputerQ::distance_to_code_1bit
// on non-centered 1-bit codes with 1 to 8 query bits, under L2 or inner product.
//
// Quantizes each residual by rounding (value - low) / delta half up, clamped
// to [0, 2^queryBits - 1]; constant residuals quantize to zero.
//
// Built with -ffp-contract=off. The fused multiply-adds of the estimate are
// written out (where the target has them) in the order the compiler's default
// contraction produced, so the row and block kernels round alike on every target.
//
// One instance serves one scan task. The constructor allocates the query-level
// buffers; setList() re-quantizes the residual into them for each list without
// allocating. Codes are read in blocks, so the per-code path has no virtual
// call and no indirect branch.
//
// Lists in RaBitQBlockLayout are scored 32 rows at a time with 4-dimension
// lookup tables of the quantized query (pshufb on AVX2 and SSSE3, tbl on
// NEON): a table entry is at most 4 * 15, so four of them sum in a byte, and
// the integer dot equals the row kernel's. The two kernels give bit-identical
// estimates. Other targets have only a scalar reference of the block kernel,
// slower than the row kernel, which scans never select (see blockKernel).
class RaBitQScorer {
public:
  struct Factors {
    float c1 = 0, c2 = 0, c34 = 0, queryToCentroidSqr = 0, queryNormSqr = 0;
  };
  struct Counts { uint64_t dot = 0, popcount = 0; };
  // Row offsets fit in 16 bits: the scan selects at most one block of rows.
  static constexpr size_t BLOCK_ROWS = 256;
  static_assert(BLOCK_ROWS % RaBitQBlockLayout::ROWS == 0);

  // Which tables setList() prepares before the first score. A kernel whose
  // tables are not prepared builds them on its first use in the list, so a
  // filtered list pays only for the kernels its selected rows need.
  enum ListKernels { LAZY = 0, ROW_KERNEL = 1, BLOCK_KERNEL = 2 };

  // The block kernel takes query bits 1 to 4.
  static bool blockQueryBits(int32_t queryBits) { return queryBits >= 1 && queryBits <= 4; }
  // This build has a vector block kernel (AVX2, SSSE3 or AArch64 NEON).
#if defined(__AVX2__) || defined(__SSSE3__) || defined(__ARM_NEON) && defined(__aarch64__)
  static constexpr bool SIMD_BLOCK_KERNEL = true;
#else
  static constexpr bool SIMD_BLOCK_KERNEL = false;
#endif
  // Whether scans of block lists use the block kernel: the planner and the
  // scan both decide by this. Without a vector block kernel they keep the row
  // kernel on the row codes.
  static bool blockKernel(int32_t queryBits) { return SIMD_BLOCK_KERNEL && blockQueryBits(queryBits); }
  // A filtered block with at least this many selected rows is scored whole by
  // the block kernel; sparser blocks use the row kernel on the row codes. The
  // selected-row count where a whole block costs no more than its rows on the
  // row kernel, measured with Tuning/BM_RaBitQBlockDensity (AVX2) over codes
  // far beyond the last-level cache: about 16 at 128 dimensions, 12 at 384 to
  // 960 and 8 to 10 at 1536. Codes in cache cross over earlier (about 8), so
  // the out-of-cache count is the safer side.
  static size_t blockMinRows(int32_t dims) {
    if (blockMinRowsOverride != 0) return blockMinRowsOverride;
    return dims <= 192 ? 16 : dims <= 1024 ? 12 : 10;
  }
  // Tests and benchmarks: replaces blockMinRows when nonzero (above
  // RaBitQBlockLayout::ROWS: never the block kernel on filtered rows).
  static inline size_t blockMinRowsOverride = 0;

private:
  const float* query;
  size_t dims;
  size_t codeBytes;
  size_t chunks;
  size_t tailWords;
  int32_t queryBits;
  bool innerProduct;
  bool useBlocks;
  float invSqrtDims;
  float maxCode;
  float gError = 0;
  Factors factors;
  RaBitQBlockLayout layout;
  // One allocation: residual floats, quantized bytes, bit planes, then the
  // block kernel's tables. Planes group by position, not by plane: first
  // `chunks` SIMD chunks of `queryBits` planes each, then `tailWords` 64-bit
  // words of `queryBits` planes each. With masked loads, the final chunk can
  // be partial and zero padded, with no tail. Tables: 32 bytes per code byte,
  // the 16 subset sums of each of its two 4-dimension groups.
  std::unique_ptr<uint8_t[]> storage;
  float* residual;
  uint8_t* quantized;
  uint8_t* planes;
  uint8_t* tables;

  template <int QB, class Ops> Counts countCode(const uint8_t* code, const Ops& ops) const;
  template <int QB, bool IP, bool Selected>
  void scoreBlock(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
  template <bool Selected>
  void dispatch(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;
  template <bool IP> void scoreBlocksImpl(const uint8_t* blocks, size_t count, float* out) const;
  // Kernels whose tables match the current list.
  mutable int prepared = 0;

  void packPlanes() const;
  void buildTables() const;
  void prepare(int kernels) const {
    if (kernels & ~prepared & ROW_KERNEL) packPlanes();
    if (kernels & ~prepared & BLOCK_KERNEL) buildTables();
    prepared |= kernels;
  }

public:
  // `blocks`: lists may be scored in RaBitQBlockLayout (query bits 1 to 4),
  // with the scalar reference on a build without a vector block kernel.
  RaBitQScorer(int32_t dims, int32_t queryBits, bool innerProduct, const float* query, bool blocks = false);

  // Prepares for the codes of the list with this (rotated-space) centroid.
  // `kernels` are prepared now; the others on first use.
  void setList(const float* centroid, int kernels = ROW_KERNEL | BLOCK_KERNEL);

  // FAISS's g_error: the L2 distance from the query to the current centroid.
  float queryToCentroid() const { return gError; }
  const Factors& listFactors() const { return factors; }
  const RaBitQBlockLayout& blockLayout() const { return layout; }
  // Quantized values, planes and the scoring kernel's integer counts; for tests.
  uint8_t quantizedValue(size_t index) const { return quantized[index]; }
  Counts counts(const uint8_t* code) const;
  uint8_t planeByte(int32_t plane, size_t index) const;
  // The block kernel's integer dots for the 32 rows of a block; for tests.
  void blockDots(const uint8_t* block, uint32_t* dots) const;
  // The kernels prepared for the current list; for tests.
  int preparedKernels() const { return prepared; }

  // out[i] = estimated distance of row i, for `count` rows at `codes`.
  void score(const uint8_t* codes, size_t codeSize, size_t count, float* out) const;
  // out[i] = estimated distance of row `rows[i]`.
  void score(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const;

  // Block kernel, which requires blockQueryBits(queryBits) and a scorer built
  // with `blocks`. out[i] = estimated distance of row i of the blocks at
  // `blocks`, for i up to `count` rounded up to whole blocks.
  void scoreBlocks(const uint8_t* blocks, size_t count, float* out) const;
  // out[i] = estimated distance of row `rows[i]` (ascending), from the same
  // rows as codes at `codes` and as blocks at `blocks`. Blocks with fewer than
  // blockMinRows selected rows are skipped by the block kernel and scored
  // through the row codes. Returns what the block kernel scored.
  struct BlockWork { size_t blocks = 0, rows = 0; };
  BlockWork score(const uint8_t* codes, size_t codeSize, const uint8_t* blocks,
                  const uint16_t* rows, size_t count, float* out) const;
  // The same, with the selected rows of block b at rows[blockStarts[b]] up to
  // rows[blockStarts[b + 1]], for `blockCount` blocks.
  BlockWork score(const uint8_t* codes, size_t codeSize, const uint8_t* blocks,
                  const uint16_t* rows, const uint16_t* blockStarts, size_t blockCount, float* out) const;
};

}  // namespace luxir
