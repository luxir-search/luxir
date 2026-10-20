// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

#include "PostingsReader.h"
#include "luxir/codec/Codec.h"
#include "luxir/index/RangeBKDWriter.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// Inclusive encoded box over (min, max). Every range-field relation is one
// such box (see RangeFieldQuery); an unbounded side is the int64 extreme.
struct RangeBoxRelation {
  int64_t lo[2];
  int64_t hi[2];

  BKDRelation compare(const int64_t (&cellLo)[2], const int64_t (&cellHi)[2]) const {
    bool inside = true;
    for (int d = 0; d < 2; d++) {
      if (cellHi[d] < lo[d] || hi[d] < cellLo[d]) return BKDRelation::OUTSIDE;
      inside = inside && lo[d] <= cellLo[d] && cellHi[d] <= hi[d];
    }
    return inside ? BKDRelation::INSIDE : BKDRelation::CROSSES;
  }

  bool matches(int64_t min, int64_t max) const {
    return lo[0] <= min && min <= hi[0] && lo[1] <= max && max <= hi[1];
  }
};

// Reads the RangeBKDWriter format.
class RangeBKDReader {
public:
  using Writer = RangeBKDWriter;
  using InnerNode = Writer::InnerNode;
  using LeafHeader = Writer::LeafHeader;
  using EstimateResult = BKDReader::EstimateResult;

  // Per-leaf decode buffers, each at least maxPointsPerLeaf() long.
  struct Scratch {
    std::span<uint32_t> docs;
    std::span<uint32_t> residuals;
    std::span<int64_t> min;
    std::span<int64_t> max;
  };

private:
  const char* points = nullptr;
  const char* innerNodeData = nullptr;
  const char* boundsData = nullptr;
  const char* leafPointerData = nullptr;
  uint64_t metaOffset = 0;
  uint64_t pointsCount = 0;
  uint32_t leavesCount = 0;
  uint16_t leafSizeMax = 0;

  template <class T>
  static T load(const char* ptr) {
    T value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  }

  [[noreturn]] static void invalid(const char* reason) {
    throw std::runtime_error(std::string("RangeBKDReader: ") + reason);
  }

  int64_t leafBound(int d, bool high, uint32_t leaf) const {
    size_t array = (size_t)(d * 2 + (high ? 1 : 0));
    return load<int64_t>(boundsData + (array * leavesCount + leaf) * sizeof(int64_t));
  }

  uint64_t leafFP(uint32_t leaf) const {
    return load<uint64_t>(leafPointerData + (uint64_t)leaf * sizeof(uint64_t));
  }

  InnerNode innerNode(uint32_t nodeId) const {
    InnerNode node = load<InnerNode>(innerNodeData + (uint64_t)(nodeId - 1) * sizeof(InnerNode));
    if (node.splitDim > 1) invalid("invalid split dimension");
    return node;
  }

  LeafHeader leafHeader(uint32_t leaf) const {
    uint64_t start = leafFP(leaf);
    uint64_t end = leafFP(leaf + 1);
    if (start >= end || end > metaOffset || end - start < Writer::LEAF_HEADER_SIZE) {
      invalid("invalid leaf extent");
    }
    LeafHeader header = load<LeafHeader>(points + start);
    if (header.count == 0 || header.count > leafSizeMax) invalid("invalid leaf point count");
    uint64_t payload = (uint64_t)Writer::LEAF_HEADER_SIZE + header.docBytes
                     + header.dimBytes[0] + header.dimBytes[1];
    if (payload != end - start) invalid("leaf payload lengths do not match extent");
    for (int d = 0; d < 2; d++) {
      if (header.bits[d] > 64
          || header.dimBytes[d] != Writer::dimByteSize(header.count, header.bits[d])) {
        invalid("invalid dimension payload");
      }
    }
    return header;
  }

  void decodeDocids(uint32_t leaf, const LeafHeader& header, std::span<uint32_t> docs) const {
    if (header.docCodec == Writer::DOC_CONTIG) {
      for (uint32_t i = 0; i < header.count; i++) docs[i] = header.docBase + i;
      return;
    }
    if (header.docCodec != Writer::DOC_FOR) invalid("reserved docid codec");
    const char* payload = points + leafFP(leaf) + Writer::LEAF_HEADER_SIZE;
    uint32_t read = IndexCodec::numericCodec.decodeWithMeta(payload, docs.data(), header.count,
                                                            header.docBits);
    if (read != header.docBytes) invalid("docid decoder byte length mismatch");
    for (uint32_t i = 0; i < header.count; i++) docs[i] += header.docBase;
  }

  void decodeDim(uint32_t leaf, const LeafHeader& header, int d, std::span<uint32_t> scratch,
                 std::span<int64_t> values) const {
    const char* payload = points + leafFP(leaf) + Writer::LEAF_HEADER_SIZE + header.docBytes
                        + (d == 1 ? header.dimBytes[0] : 0);
    uint64_t base = (uint64_t)header.base[d];
    if (header.bits[d] <= Writer::MAX_PACKED_BITS) {
      uint32_t read = IndexCodec::numericCodec.decodeWithMeta(payload, scratch.data(), header.count,
                                                              header.bits[d]);
      if (read != header.dimBytes[d]) invalid("dimension decoder byte length mismatch");
      for (uint32_t i = 0; i < header.count; i++) values[i] = (int64_t)(base + scratch[i]);
    } else {
      for (uint32_t i = 0; i < header.count; i++) {
        values[i] = (int64_t)(base + load<uint64_t>(payload + (uint64_t)i * sizeof(uint64_t)));
      }
    }
  }

  // Walks the tree, calling inside(firstLeaf, leaves) for runs of leaves the
  // relation fully covers and crossing(leaf) for leaves it partly covers.
  template <class Inside, class Crossing>
  void visit(const RangeBoxRelation& relation, Inside&& inside, Crossing&& crossing) const {
    auto walk = [&](auto&& self, uint32_t nodeId, uint32_t leaves, uint32_t firstLeaf,
                    int64_t (&cellLo)[2], int64_t (&cellHi)[2]) -> void {
      BKDRelation cell = relation.compare(cellLo, cellHi);
      if (cell == BKDRelation::OUTSIDE) return;
      if (cell == BKDRelation::INSIDE) {
        inside(firstLeaf, leaves);
        return;
      }
      if (leaves == 1) {
        int64_t leafLo[2] = {leafBound(0, false, firstLeaf), leafBound(1, false, firstLeaf)};
        int64_t leafHi[2] = {leafBound(0, true, firstLeaf), leafBound(1, true, firstLeaf)};
        BKDRelation tight = relation.compare(leafLo, leafHi);
        if (tight == BKDRelation::INSIDE) inside(firstLeaf, 1);
        else if (tight == BKDRelation::CROSSES) crossing(firstLeaf);
        return;
      }
      InnerNode node = innerNode(nodeId);
      uint32_t leftLeaves = Writer::numLeftLeaves(leaves);
      int d = node.splitDim;
      int64_t saved = cellHi[d];
      cellHi[d] = node.splitValue;
      self(self, nodeId * 2, leftLeaves, firstLeaf, cellLo, cellHi);
      cellHi[d] = saved;
      saved = cellLo[d];
      cellLo[d] = node.splitValue;
      self(self, nodeId * 2 + 1, leaves - leftLeaves, firstLeaf + leftLeaves, cellLo, cellHi);
      cellLo[d] = saved;
    };
    int64_t cellLo[2] = {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min()};
    int64_t cellHi[2] = {std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()};
    walk(walk, 1, leavesCount, 0, cellLo, cellHi);
  }

public:
  RangeBKDReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) {
    static_assert(std::endian::native == std::endian::little);
    if (fieldInfo.pointsMetaOff <= 0) invalid("points index is absent");
    InputStream input = postingsReader.getInputStream(fieldInfo.pointsLoc.filenum());
    uint64_t pointsOff = fieldInfo.pointsLoc.offset();
    metaOffset = (uint64_t)fieldInfo.pointsMetaOff;
    if (pointsOff > (uint64_t)input.size() || metaOffset > (uint64_t)input.size() - pointsOff) {
      invalid("points location is out of bounds");
    }
    points = input.ptr((int64_t)pointsOff);
    const char* fileEnd = input.ptr(input.size());
    if ((uint64_t)(fileEnd - points) - metaOffset < Writer::FIXED_HEADER_SIZE) {
      invalid("truncated metadata header");
    }
    auto header = load<Writer::FixedHeader>(points + metaOffset);
    if (header.magic != Writer::MAGIC || header.version != Writer::VERSION
        || header.formatKind != Writer::FORMAT_KIND || header.bytesPerDim != Writer::BYTES_PER_DIM
        || header.numDims != Writer::NUM_DIMS) {
      invalid("unsupported header");
    }
    if (header.leafCount == 0 || header.maxPointsPerLeaf == 0 || header.pointCount == 0) {
      invalid("empty index");
    }
    leavesCount = header.leafCount;
    leafSizeMax = header.maxPointsPerLeaf;
    pointsCount = header.pointCount;
    if (pointsCount / leafSizeMax + (pointsCount % leafSizeMax != 0) != leavesCount) {
      invalid("leafCount does not match pointCount");
    }

    uint64_t directoryFileOffset = (pointsOff + metaOffset + Writer::FIXED_HEADER_SIZE + 7) & ~(uint64_t)7;
    uint64_t directoryOff = directoryFileOffset - pointsOff;
    uint64_t innerBytes = (uint64_t)(leavesCount - 1) * sizeof(InnerNode);
    uint64_t boundsBytes = (uint64_t)leavesCount * sizeof(int64_t) * 4;
    uint64_t pointerBytes = ((uint64_t)leavesCount + 1) * sizeof(uint64_t);
    if (directoryOff + innerBytes + boundsBytes + pointerBytes > (uint64_t)(fileEnd - points)) {
      invalid("truncated metadata arrays");
    }
    innerNodeData = points + directoryOff;
    boundsData = innerNodeData + innerBytes;
    leafPointerData = boundsData + boundsBytes;
    if (leafFP(0) != 0 || leafFP(leavesCount) != metaOffset) invalid("bad leaf pointers");
  }

  uint64_t pointCount() const { return pointsCount; }
  uint16_t maxPointsPerLeaf() const { return leafSizeMax; }

  Scratch scratch(MemPool& pool) const {
    return {pool.make_span<uint32_t>(leafSizeMax), pool.make_span<uint32_t>(leafSizeMax),
            pool.make_span<int64_t>(leafSizeMax), pool.make_span<int64_t>(leafSizeMax)};
  }

  // Emits the docid of every matching point, sorted only within leaves.
  // emitRun(begin, end) receives contiguous runs from fully covered leaves.
  template <class EmitDoc, class EmitRun>
  void intersect(const RangeBoxRelation& relation, Scratch scratch, EmitDoc&& emitDoc,
                 EmitRun&& emitRun) const {
    auto emitLeaf = [&](uint32_t leaf) {
      LeafHeader header = leafHeader(leaf);
      if (header.docCodec == Writer::DOC_CONTIG) {
        emitRun((int32_t)header.docBase, (int32_t)(header.docBase + header.count));
        return;
      }
      decodeDocids(leaf, header, scratch.docs);
      for (uint32_t i = 0; i < header.count; i++) emitDoc((int32_t)scratch.docs[i]);
    };
    visit(relation,
        [&](uint32_t firstLeaf, uint32_t leaves) {
          for (uint32_t leaf = firstLeaf; leaf < firstLeaf + leaves; leaf++) emitLeaf(leaf);
        },
        [&](uint32_t leaf) {
          LeafHeader header = leafHeader(leaf);
          decodeDocids(leaf, header, scratch.docs);
          decodeDim(leaf, header, 0, scratch.residuals, scratch.min);
          decodeDim(leaf, header, 1, scratch.residuals, scratch.max);
          for (uint32_t i = 0; i < header.count; i++) {
            if (relation.matches(scratch.min[i], scratch.max[i])) emitDoc((int32_t)scratch.docs[i]);
          }
        });
  }

  // Inside leaves count fully; crossing leaves count half in the estimate
  // and fully in the upper bound.
  EstimateResult estimateIntersect(const RangeBoxRelation& relation) const {
    EstimateResult result{0, 0};
    visit(relation,
        [&](uint32_t firstLeaf, uint32_t leaves) {
          for (uint32_t leaf = firstLeaf; leaf < firstLeaf + leaves; leaf++) {
            uint64_t count = leafHeader(leaf).count;
            result.estimatedCount += count;
            result.upperBound += count;
          }
        },
        [&](uint32_t leaf) {
          uint64_t count = leafHeader(leaf).count;
          result.estimatedCount += (count + 1) / 2;
          result.upperBound += count;
        });
    return result;
  }

  // Exact number of matching points.
  uint64_t countIntersect(const RangeBoxRelation& relation, Scratch scratch) const {
    uint64_t count = 0;
    visit(relation,
        [&](uint32_t firstLeaf, uint32_t leaves) {
          for (uint32_t leaf = firstLeaf; leaf < firstLeaf + leaves; leaf++) {
            count += leafHeader(leaf).count;
          }
        },
        [&](uint32_t leaf) {
          LeafHeader header = leafHeader(leaf);
          decodeDim(leaf, header, 0, scratch.residuals, scratch.min);
          decodeDim(leaf, header, 1, scratch.residuals, scratch.max);
          for (uint32_t i = 0; i < header.count; i++) {
            if (relation.matches(scratch.min[i], scratch.max[i])) count++;
          }
        });
    return count;
  }
};

} // namespace luxir
