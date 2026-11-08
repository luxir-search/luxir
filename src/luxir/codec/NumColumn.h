// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/codec/NumColumnFormat.h"
#include "luxir/codec/LinearPack.h"

namespace luxir {

// Shared numeric-column physical decode layer. Descriptors are read in place.
//
// The block-meta array must be 8-aligned - NumBlockInfo is a naturally aligned
// struct read straight off the mapping. NumColumnWriter::finish aligns it in
// the stream, so anything that RELOCATES a finished column has to preserve
// that; OrdMap appends columns into one buffer and pads to 8 (OrdMapImpl.h).
// The constructors assert it so a new relocator fails loudly rather than
// silently reading misaligned descriptors.
//
// WithGcd is a compile-time property of the column, not of a block: monotonic
// columns are written with gcd pinned to 1 (the slope carries any regular
// step), so their decode drops the field load and the multiply entirely rather
// than branching around them.
template <bool WithGcd>
class NumColumnT {
public:
  static constexpr uint32_t BLOCK_SIZE = NumColumnFormat::BLOCK_SIZE;
  static constexpr uint32_t BULK_SIZE = NumColumnFormat::BULK_SIZE;
  static constexpr int64_t ENDINDEX = std::numeric_limits<int64_t>::max();

private:
  const char* blocks = nullptr;
  const char* blockMeta = nullptr;
  int64_t nValues = 0;

  static int64_t reconstruct(const NumBlockInfo& info, uint64_t rankInBlock,
                             uint64_t residual) {
    // Constant blocks are the common case for unordered numeric data, and
    // slopeTerm costs two multiplies that produce zero for them. The test is
    // per-block-shaped so it predicts; decodeFrame splits the same way.
    uint64_t scaled = residual;
    if (info.scaledSlope != 0) {
      scaled += (uint64_t)NumColumnFormat::slopeTerm(rankInBlock,
                                                     info.scaledSlope);
    }
    if constexpr (WithGcd) {
      return (int64_t)(info.baseBits + info.gcd * scaled);
    } else {
      assert(info.gcd == 1);
      return (int64_t)(info.baseBits + scaled);
    }
  }

public:
  NumColumnT() = default;

  NumColumnT(const char* blocks, const char* blockMeta, int64_t nValues)
      : blocks(blocks), blockMeta(blockMeta), nValues(nValues) {
    // Descriptors are read in place, so a relocated column that lost its
    // 8-alignment lands here rather than in a misaligned load downstream.
    assert(((uintptr_t)blockMeta & (alignof(NumBlockInfo) - 1)) == 0);
  }

  int64_t numValues() const {
    return nValues;
  }

  int64_t numBlocks() const {
    return (nValues + BLOCK_SIZE - 1) / BLOCK_SIZE;
  }

  // Read in place. NumBlockInfo is unaligned, so field access compiles to the
  // same loads a copy would make, without materializing the struct.
  const NumBlockInfo& blockInfo(int64_t blockNum) const {
    assert(blockNum >= 0 && blockNum < numBlocks());
    return ((const NumBlockInfo*)blockMeta)[blockNum];
  }

  std::optional<std::pair<uint64_t, uint64_t>> affineBlock(int64_t blockNum) const {
    const auto& info = blockInfo(blockNum);
    if (info.bits() != 0 || ((uint64_t)info.scaledSlope & ((1ULL << NumColumnFormat::SLOPE_SHIFT) - 1))) return {};
    uint64_t step = (uint64_t)(info.scaledSlope >> NumColumnFormat::SLOPE_SHIFT);
    if constexpr (WithGcd) step *= info.gcd;
    return std::pair(info.baseBits, step);
  }

  int32_t valuesInBlock(int64_t blockNum) const {
    assert(blockNum >= 0 && blockNum < numBlocks());
    int64_t start = blockNum * (int64_t)BLOCK_SIZE;
    return (int32_t)std::min<int64_t>(BLOCK_SIZE, nValues - start);
  }

  // Rank intervals this block's encoding proves cannot match / all match
  // [lo, hi]; nullopt when the encoding proves nothing beyond its zones.
  std::optional<NumColumnFormat::RankBounds> rankBounds(
      int64_t blockNum, int64_t lo, int64_t hi) const {
    const auto& info = blockInfo(blockNum);
    if (!info.linearFit()) return std::nullopt;
    return NumColumnFormat::linearRankBounds(
        info, valueAt(blockNum * (int64_t)BLOCK_SIZE),
        (uint32_t)valuesInBlock(blockNum), lo, hi);
  }

  int64_t valueAt(int64_t rank) const {
    assert(rank >= 0 && rank < nValues);
    uint64_t rankInBlock = (uint64_t)rank % BLOCK_SIZE;
    const NumBlockInfo& info =
        blockInfo((int64_t)((uint64_t)rank / BLOCK_SIZE));
    const char* payload = blocks + info.payloadOffset();
    if (info.bits() > NumColumnFormat::MAX_PACKED_BITS) {
      return loadUnaligned<int64_t>(payload + rankInBlock * sizeof(int64_t));
    }
    uint64_t residual = LinearPack::select64(
        payload, rankInBlock, info.bits(), LinearPack::mask64(info.bits()));
    return reconstruct(info, rankInBlock, residual);
  }

  // Values at rank-1 and rank, with rank-1 defined as 0 at rank 0. This is how
  // monotonic columns are actually read - a doc's value range is the adjacent
  // pair - and the two ranks share a block except at a boundary, so resolve the
  // descriptor once instead of paying for it twice.
  std::pair<int64_t, int64_t> valuesAt(int64_t rank) const {
    assert(rank >= 0 && rank < nValues);
    // Both cases where the pair does not share a descriptor: rank 0, which has
    // no predecessor, and a block boundary, where rank - 1 is the previous
    // block's last value. rank 0 has rankInBlock 0, so one test covers both.
    // Only rank 0 yields 0; a boundary reads each side normally.
    uint64_t rankInBlock = (uint64_t)rank % BLOCK_SIZE;
    if (rankInBlock == 0) {
      return {rank > 0 ? valueAt(rank - 1) : 0, valueAt(rank)};
    }
    const NumBlockInfo& info =
        blockInfo((int64_t)((uint64_t)rank / BLOCK_SIZE));
    const char* payload = blocks + info.payloadOffset();
    if (info.bits() > NumColumnFormat::MAX_PACKED_BITS) {
      int64_t pair[2];
      memcpy(pair, payload + (rankInBlock - 1) * sizeof(int64_t), sizeof(pair));
      return {pair[0], pair[1]};
    }
    uint64_t mask = LinearPack::mask64(info.bits());
    uint64_t prev = LinearPack::select64(payload, rankInBlock - 1, info.bits(), mask);
    uint64_t cur = LinearPack::select64(payload, rankInBlock, info.bits(), mask);
    return {reconstruct(info, rankInBlock - 1, prev),
            reconstruct(info, rankInBlock, cur)};
  }

  // Decode comparable residuals without restoring the block's base/gcd.
  // Returns the global rank of out[0] and the number of valid values in count.
  int64_t decodeResidualFrame(int64_t rank, uint32_t* out,
                              uint32_t& count) const {
    assert(rank >= 0 && rank < nValues);
    int64_t start = rank / BULK_SIZE * BULK_SIZE;
    const auto& info = blockInfo(start / BLOCK_SIZE);
    assert(info.residualComparable());
    count = (uint32_t)std::min<int64_t>(BULK_SIZE, nValues - start);
    LinearPack::unpack128(
        blocks + info.payloadOffset(), (uint64_t)start % BLOCK_SIZE, count,
        info.bits(), LinearPack::mask32(info.bits()), out);
    return start;
  }

  // Decode the 128-aligned frame containing rank. Returns the global rank of
  // out[0] and stores the number of valid values in count.
  //
  // Deliberately out of line. This runs once per 128 values, but it carries
  // every format variant (raw, packed-narrow with three scale-loop shapes,
  // packed-wide), so inlining it into a caller's per-value loop bloats that
  // loop and spills its cursor state to the stack. Measured on dense numeric
  // faceting: inlined, the per-value path reloads decodedStart/decodedEnd from
  // stack every iteration and the whole facet is ~25% slower.
  LUXIR_NOINLINE int64_t decodeFrame(int64_t rank, int64_t* out,
                                     uint32_t& count) const {
    assert(rank >= 0 && rank < nValues);
    int64_t start = rank / BULK_SIZE * BULK_SIZE;
    int64_t blockNum = start / BLOCK_SIZE;
    uint64_t rankInBlock = (uint64_t)start % BLOCK_SIZE;
    const NumBlockInfo& info = blockInfo(blockNum);
    count = (uint32_t)std::min<int64_t>(BULK_SIZE, nValues - start);
    const char* payload = blocks + info.payloadOffset();

    if (info.bits() > NumColumnFormat::MAX_PACKED_BITS) {
      memcpy(out, payload + rankInBlock * sizeof(int64_t),
             count * sizeof(int64_t));
      return start;
    }

    if (info.bits() <= 32) {
      uint32_t residuals[BULK_SIZE];
      LinearPack::unpack128(payload, rankInBlock, count, info.bits(),
                            LinearPack::mask32(info.bits()), residuals);
      if (info.gcd == 1 && info.scaledSlope == 0) {
        for (uint32_t i = 0; i < count; i++) {
          out[i] = (int64_t)(info.baseBits + residuals[i]);
        }
      } else if (info.scaledSlope == 0) {
        for (uint32_t i = 0; i < count; i++) {
          out[i] = (int64_t)(info.baseBits +
              info.gcd * (uint64_t)residuals[i]);
        }
      } else {
        for (uint32_t i = 0; i < count; i++) {
          out[i] = reconstruct(info, rankInBlock + i, residuals[i]);
        }
      }
      return start;
    }

    uint64_t mask = LinearPack::mask64(info.bits());
    for (uint32_t i = 0; i < count; i++) {
      uint64_t residual = LinearPack::select64(
          payload, rankInBlock + i, info.bits(), mask);
      out[i] = reconstruct(info, rankInBlock + i, residual);
    }
    return start;
  }

  class Bulk {
    const NumColumnT& column;
    int64_t index_ = -1;
    int64_t decodedStart = -1;
    int64_t decodedEnd = -1;
    int64_t decoded[BULK_SIZE];

    void decode(int64_t index) {
      uint32_t count;
      decodedStart = column.decodeFrame(index, decoded, count);
      decodedEnd = decodedStart + count;
    }

  public:
    explicit Bulk(const NumColumnT& column) : column(column) {}

    int64_t index() const {
      return index_;
    }

    int64_t value() const {
      assert(index_ >= decodedStart && index_ < decodedEnd);
      return decoded[index_ - decodedStart];
    }

    int64_t next() {
      if (++index_ >= column.numValues()) {
        index_ = ENDINDEX;
        return index_;
      }
      if (index_ >= decodedEnd) decode(index_);
      return index_;
    }

    int64_t advance(int64_t target) {
      assert(target > index_);
      index_ = target;
      if (index_ >= column.numValues()) {
        index_ = ENDINDEX;
        return index_;
      }
      if (index_ < decodedStart || index_ >= decodedEnd) decode(index_);
      return index_;
    }

    // Always inlined, like the doc iterator's advance: the column walks call
    // it once per value, and only the frame decode belongs out of line.
    LUXIR_INLINE int64_t valueAt(int64_t index) {
      assert(index >= 0 && index < column.numValues());
      if (index < decodedStart || index >= decodedEnd) decode(index);
      return decoded[index - decodedStart];
    }
  };
};

// General numeric columns: values are arbitrary, so a per-block common divisor
// is worth finding (coarse-granularity dates are the shape that pays for it).
using NumColumn = NumColumnT<true>;
// Monotonic columns: written with gcd pinned to 1, so the decode drops it.
using MonoColumn = NumColumnT<false>;


} // namespace luxir
