// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/codec/NumColumnFormat.h"
#include "luxir/codec/LinearPack.h"

namespace luxir {

// Decode of the block kinds other than FOR (NumBlockKind; layouts in
// NumBlockPlanner). Only general columns write them, so only NumColumnT<true>
// reaches here. `count` is the block's value count, which locates what
// follows the packed slots.
class NumBlockCodecs {
  static constexpr uint32_t BULK_SIZE = NumColumnFormat::BULK_SIZE;

  // The first exception at or after rankInBlock.
  static uint64_t lowerException(const char* ranks, uint64_t exceptions,
                                 uint64_t rankInBlock) {
    uint64_t lo = 0;
    uint64_t hi = exceptions;
    while (lo < hi) {
      uint64_t mid = lo + (hi - lo) / 2;
      if (loadUnaligned<uint16_t>(ranks + mid * sizeof(uint16_t)) < rankInBlock) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

public:
  static uint64_t frameCount(uint64_t count) {
    return (count + BULK_SIZE - 1) / BULK_SIZE;
  }

  // The packed slots: DELTA deltas, DICT codes, ALP scaled integers,
  // FOR_PATCHED residuals.
  static const char* slots(const NumBlockInfo& info, const char* payload,
                           uint64_t count) {
    switch (info.kind()) {
    case NumBlockKind::DELTA:
      return payload
          + LinearPack::packedByteSize(frameCount(count), info.anchorBits());
    case NumBlockKind::DICT:
      return payload + info.dictSize() * sizeof(int64_t);
    default:
      return payload;
    }
  }

  // Calls f(rankInBlock, value) for each exception of a patched block in
  // [rankInBlock, rankInBlock + n), by rank.
  template <class F>
  static void forEachException(const NumBlockInfo& info, const char* payload,
                               uint64_t count, uint64_t rankInBlock,
                               uint64_t n, F&& f) {
    uint64_t exceptions = info.exceptions();
    if (exceptions == 0) return;
    const char* ranks = payload + LinearPack::packedByteSize(count, info.bits());
    const char* values = ranks + exceptions * sizeof(uint16_t);
    for (uint64_t x = lowerException(ranks, exceptions, rankInBlock);
         x < exceptions; x++) {
      uint64_t rank = loadUnaligned<uint16_t>(ranks + x * sizeof(uint16_t));
      if (rank >= rankInBlock + n) break;
      f(rank, loadUnaligned<int64_t>(values + x * sizeof(int64_t)));
    }
  }

  // n slots from the frame-aligned rankInBlock.
  static void unpackSlots(const char* packed, uint64_t rankInBlock,
                          uint32_t n, uint8_t bits, uint64_t* out) {
    assert(rankInBlock % BULK_SIZE == 0 && n <= BULK_SIZE);
    if (bits <= 32) {
      uint32_t narrow[BULK_SIZE];
      LinearPack::unpack128(packed, rankInBlock, n, bits,
                            LinearPack::mask32(bits), narrow);
      for (uint32_t i = 0; i < n; i++) out[i] = narrow[i];
      return;
    }
    LinearPack::unpack128(packed, rankInBlock, n, bits,
                          LinearPack::mask64(bits), out);
  }

  static int64_t valueAt(const NumBlockInfo& info, const char* payload,
                         uint64_t count, uint64_t rankInBlock) {
    const char* packed = slots(info, payload, count);
    uint8_t bits = info.bits();
    NumBlockKind kind = info.kind();
    if (kind == NumBlockKind::DELTA) {
      // The prefix sum of the rank's frame up to the rank.
      uint64_t frameStart = rankInBlock / BULK_SIZE * BULK_SIZE;
      int64_t out[BULK_SIZE];
      uint32_t n = (uint32_t)(rankInBlock - frameStart + 1);
      decodeFrame(info, payload, count, frameStart, n, out);
      return out[n - 1];
    }
    uint64_t slot = LinearPack::select64(packed, rankInBlock, bits,
                                         LinearPack::mask64(bits));
    if (kind == NumBlockKind::DICT) {
      assert(slot < info.dictSize());
      return loadUnaligned<int64_t>(payload + slot * sizeof(int64_t));
    }
    std::optional<int64_t> exception;
    forEachException(info, payload, count, rankInBlock, 1,
                     [&](uint64_t, int64_t value) { exception = value; });
    if (exception) return *exception;
    if (kind == NumBlockKind::FOR_PATCHED) {
      return (int64_t)(info.baseBits + info.gcd * slot);
    }
    return NumColumnFormat::alpDecode((int64_t)(info.baseBits + slot),
                                      info.alpExponent(), info.alpFactor(),
                                      kind);
  }

  // The values at rankInBlock - 1 and rankInBlock (> 0): one prefix sum for
  // a DELTA pair inside a frame, two point reads otherwise.
  static std::pair<int64_t, int64_t> valuesAt(const NumBlockInfo& info,
                                              const char* payload,
                                              uint64_t count,
                                              uint64_t rankInBlock) {
    assert(rankInBlock > 0);
    if (info.kind() == NumBlockKind::DELTA && rankInBlock % BULK_SIZE != 0) {
      uint64_t frameStart = rankInBlock / BULK_SIZE * BULK_SIZE;
      int64_t out[BULK_SIZE];
      uint32_t n = (uint32_t)(rankInBlock - frameStart + 1);
      decodeFrame(info, payload, count, frameStart, n, out);
      return {out[n - 2], out[n - 1]};
    }
    return {valueAt(info, payload, count, rankInBlock - 1),
            valueAt(info, payload, count, rankInBlock)};
  }

  // The n values from the frame-aligned rankInBlock.
  static void decodeFrame(const NumBlockInfo& info, const char* payload,
                          uint64_t count, uint64_t rankInBlock, uint32_t n,
                          int64_t* out) {
    assert(rankInBlock + n <= count);
    uint64_t slots[BULK_SIZE];
    unpackSlots(NumBlockCodecs::slots(info, payload, count), rankInBlock, n,
                info.bits(), slots);
    switch (info.kind()) {
    case NumBlockKind::DELTA: {
      uint8_t anchorBits = info.anchorBits();
      uint64_t value = info.gcd
          + LinearPack::select64(payload, rankInBlock / BULK_SIZE, anchorBits,
                                 LinearPack::mask64(anchorBits));
      out[0] = (int64_t)value;
      for (uint32_t i = 1; i < n; i++) {
        value += info.baseBits + slots[i];
        out[i] = (int64_t)value;
      }
      return;
    }
    case NumBlockKind::DICT:
      for (uint32_t i = 0; i < n; i++) {
        assert(slots[i] < info.dictSize());
        out[i] = loadUnaligned<int64_t>(payload + slots[i] * sizeof(int64_t));
      }
      return;
    case NumBlockKind::FOR_PATCHED: {
      uint64_t base = info.baseBits;
      uint64_t gcd = info.gcd;
      for (uint32_t i = 0; i < n; i++) out[i] = (int64_t)(base + gcd * slots[i]);
      break;
    }
    default: {
      // ALP: branch free over the slots, with the scale hoisted so the loop
      // vectorizes; the exceptions are patched in after.
      assert(info.kind() == NumBlockKind::ALP_DOUBLE
             || info.kind() == NumBlockKind::ALP_FLOAT);
      double factor = NumColumnFormat::ALP_POW10[info.alpFactor()];
      double exponent = NumColumnFormat::ALP_POW10[info.alpExponent()];
      uint64_t base = info.baseBits;
      if (info.kind() == NumBlockKind::ALP_FLOAT) {
        for (uint32_t i = 0; i < n; i++) {
          out[i] = NumColumnFormat::alpDecodeFloat(
              NumColumnFormat::alpNumber((int64_t)(base + slots[i])), factor,
              exponent);
        }
      } else {
        for (uint32_t i = 0; i < n; i++) {
          out[i] = NumColumnFormat::alpDecodeDouble(
              NumColumnFormat::alpNumber((int64_t)(base + slots[i])), factor,
              exponent);
        }
      }
      break;
    }
    }
    forEachException(info, payload, count, rankInBlock, n,
                     [&](uint64_t rank, int64_t value) {
                       out[rank - rankInBlock] = value;
                     });
  }

  // The slots of a slotComparable non-FOR block that decode into [lo, hi]
  // (exceptions aside): every such kind decodes nondecreasingly in its
  // slot, so the slots form one interval.
  static NumColumnFormat::ResidualBounds slotBounds(
      const NumBlockInfo& info, const char* payload, int64_t lo, int64_t hi) {
    assert(info.slotComparable() && info.kind() != NumBlockKind::FOR);
    using Bounds = NumColumnFormat::ResidualBounds;
    if (hi < lo) return Bounds::empty();
    uint8_t bits = info.bits();
    NumBlockKind kind = info.kind();
    if (kind == NumBlockKind::FOR_PATCHED) {
      return NumColumnFormat::latticeBounds(info.baseBits, info.gcd, bits, lo,
                                            hi);
    }
    // The first slot below `limit` (a power of two at most 2^32) whose
    // value is not before v.
    auto partition = [](uint64_t limit, auto before) {
      uint64_t first = 0;
      uint64_t last = limit;
      while (first < last) {
        uint64_t mid = first + (last - first) / 2;
        if (before(mid)) first = mid + 1;
        else last = mid;
      }
      return first;
    };
    uint64_t lower, end;
    if (kind == NumBlockKind::DICT) {
      auto entry = [&](uint64_t code) {
        return loadUnaligned<int64_t>(payload + code * sizeof(int64_t));
      };
      uint64_t size = info.dictSize();
      lower = partition(size, [&](uint64_t c) { return entry(c) < lo; });
      end = partition(size, [&](uint64_t c) { return entry(c) <= hi; });
    } else {
      uint64_t limit = 1ULL << bits;
      auto value = [&](uint64_t slot) {
        return NumColumnFormat::alpDecode((int64_t)(info.baseBits + slot),
                                          info.alpExponent(),
                                          info.alpFactor(), kind);
      };
      lower = partition(limit, [&](uint64_t s) { return value(s) < lo; });
      end = partition(limit, [&](uint64_t s) { return value(s) <= hi; });
    }
    if (lower >= end) return Bounds::empty();
    return {(uint32_t)lower, (uint32_t)(end - 1)};
  }
};

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

  bool has_value() const noexcept { return blocks != nullptr; }

  int64_t numValues() const {
    return nValues;
  }

  // Bytes before the block directory, including alignment padding.
  uint64_t payloadBytes() const { return (uint64_t)(blockMeta - blocks); }

  // Packed payload, alignment and block directory; excludes optional zones.
  uint64_t blockBytes() const {
    return payloadBytes() + (uint64_t)numBlocks() * sizeof(NumBlockInfo);
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
    if (!info.forPacked() || info.bits() != 0 || ((uint64_t)info.scaledSlope & ((1ULL << NumColumnFormat::SLOPE_SHIFT) - 1))) return {};
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
    if (!info.forPacked()) {
      if constexpr (WithGcd) {
        if (!info.raw()) {
          return NumBlockCodecs::valueAt(
              info, payload,
              (uint64_t)valuesInBlock((int64_t)((uint64_t)rank / BLOCK_SIZE)),
              rankInBlock);
        }
      }
      assert(info.raw());
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
    if (!info.forPacked()) {
      if constexpr (WithGcd) {
        if (!info.raw()) {
          return NumBlockCodecs::valuesAt(
              info, payload,
              (uint64_t)valuesInBlock((int64_t)((uint64_t)rank / BLOCK_SIZE)),
              rankInBlock);
        }
      }
      assert(info.raw());
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

  // Decode a slotComparable block's packed slots without restoring values:
  // FOR residuals, DICT codes, ALP scaled integers less their least,
  // FOR_PATCHED residuals (0 at an exception). Returns the global rank of
  // out[0] and the number of valid values in count.
  int64_t decodeResidualFrame(int64_t rank, uint32_t* out,
                              uint32_t& count) const {
    assert(rank >= 0 && rank < nValues);
    int64_t start = rank / BULK_SIZE * BULK_SIZE;
    int64_t blockNum = start / BLOCK_SIZE;
    const auto& info = blockInfo(blockNum);
    assert(info.slotComparable());
    count = (uint32_t)std::min<int64_t>(BULK_SIZE, nValues - start);
    const char* packed = blocks + info.payloadOffset();
    if constexpr (WithGcd) {
      if (!info.forPacked()) {
        packed = NumBlockCodecs::slots(info, packed,
                                       (uint64_t)valuesInBlock(blockNum));
      }
    }
    LinearPack::unpack128(packed, (uint64_t)start % BLOCK_SIZE, count,
                          info.bits(), LinearPack::mask32(info.bits()), out);
    return start;
  }

  // The slots of a slotComparable block whose values (exceptions aside) lie
  // in [lo, hi], as an interval; nullopt for a block that is not.
  std::optional<NumColumnFormat::ResidualBounds> slotBounds(
      int64_t blockNum, int64_t lo, int64_t hi) const {
    const auto& info = blockInfo(blockNum);
    if (!info.slotComparable()) return std::nullopt;
    if (info.residualComparable()) {
      return NumColumnFormat::residualBounds(info, lo, hi);
    }
    if constexpr (WithGcd) {
      return NumBlockCodecs::slotBounds(info, blocks + info.payloadOffset(),
                                        lo, hi);
    }
    assert(false);
    return std::nullopt;
  }

  // Calls f(rank, value) for each exception of a patched block in [rank,
  // rank + n), all in one block, by rank.
  template <class F>
  void forEachException(int64_t rank, uint32_t n, F&& f) const {
    assert(rank >= 0 && rank + n <= nValues);
    int64_t blockNum = rank / BLOCK_SIZE;
    const auto& info = blockInfo(blockNum);
    if constexpr (WithGcd) {
      if (info.exceptions() == 0) return;
      int64_t blockStart = blockNum * (int64_t)BLOCK_SIZE;
      NumBlockCodecs::forEachException(
          info, blocks + info.payloadOffset(),
          (uint64_t)valuesInBlock(blockNum), (uint64_t)(rank - blockStart), n,
          [&](uint64_t rankInBlock, int64_t value) {
            f(blockStart + (int64_t)rankInBlock, value);
          });
    } else {
      assert(info.exceptions() == 0);
    }
  }

  // Decode the 128-aligned frame containing rank. Returns the global rank of
  // out[0] and stores the number of valid values in count.
  //
  // Deliberately out of line. This runs once per 128 values, but it carries
  // every format variant (raw, packed-narrow with three scale-loop shapes,
  // packed-wide, the other block kinds), so inlining it into a caller's
  // per-value loop bloats that loop and spills its cursor state to the
  // stack. Measured on dense numeric faceting: inlined, the per-value path
  // reloads decodedStart/decodedEnd from stack every iteration and the whole
  // facet is ~25% slower.
  LUXIR_NOINLINE int64_t decodeFrame(int64_t rank, int64_t* out,
                                     uint32_t& count) const {
    assert(rank >= 0 && rank < nValues);
    int64_t start = rank / BULK_SIZE * BULK_SIZE;
    int64_t blockNum = start / BLOCK_SIZE;
    uint64_t rankInBlock = (uint64_t)start % BLOCK_SIZE;
    const NumBlockInfo& info = blockInfo(blockNum);
    count = (uint32_t)std::min<int64_t>(BULK_SIZE, nValues - start);
    const char* payload = blocks + info.payloadOffset();

    if (!info.forPacked()) {
      if constexpr (WithGcd) {
        if (!info.raw()) {
          NumBlockCodecs::decodeFrame(info, payload,
                                      (uint64_t)valuesInBlock(blockNum),
                                      rankInBlock, count, out);
          return start;
        }
      }
      assert(info.raw());
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
