// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "luxir/codec/Codec.h"
#include "luxir/index/PointsWriter.h"
#include "luxir/store/OutputStream.h"
#include "luxir/util/thread.h"

namespace luxir {

// Two-dimensional int64 BKD over the (min, max) endpoints of a range field.
// The tree shape matches the geo BKDWriter (balanced heap of full leaves,
// split on the wider dimension, tight per-leaf bounds in the directory); only
// the scalar width differs, so it is a separate format kind. Each leaf stores
// its docids (CONTIG or FOR) and, per dimension, residuals against the leaf
// minimum: FOR-packed when they fit 32 bits, else raw uint64.
class RangeBKDWriter {
public:
  static constexpr uint32_t MAGIC = PointsWriter::MAGIC;
  static constexpr uint16_t VERSION = 1;
  static constexpr uint8_t FORMAT_KIND = 3;
  static constexpr uint8_t BYTES_PER_DIM = 8;
  static constexpr uint8_t NUM_DIMS = 2;
  static constexpr uint16_t DEFAULT_MAX_POINTS_PER_LEAF =
      PointsWriter::DEFAULT_MAX_POINTS_PER_LEAF;
  static constexpr uint32_t FIXED_HEADER_SIZE = 32;
  static constexpr uint32_t LEAF_HEADER_SIZE = 40;
  static constexpr uint8_t DOC_CONTIG = PointsWriter::DOC_CONTIG;
  static constexpr uint8_t DOC_FOR = PointsWriter::DOC_FOR;
  // Residuals wider than this are stored raw.
  static constexpr uint8_t MAX_PACKED_BITS = 32;
  static constexpr uint32_t SPAWN_MIN_POINTS = 32768;

  struct Point {
    int64_t min;
    int64_t max;
    int32_t docid;

    bool operator==(const Point&) const = default;
  };

  struct Data {
    seg_location pointsLoc;
    int64_t pointsMetaOff;
    uint64_t pointCount;
    uint32_t leafCount;
  };

  struct __attribute__((packed)) FixedHeader {
    uint32_t magic;
    uint16_t version;
    uint8_t formatKind;
    uint8_t bytesPerDim;
    uint8_t numDims;
    uint8_t padding[3];
    uint32_t leafCount;
    uint16_t maxPointsPerLeaf;
    uint16_t padding2;
    uint64_t pointCount;
    uint32_t padding3;
  };

  // Logical nodes are 1-indexed in heap order; entry nodeId - 1 holds nodeId.
  struct __attribute__((packed)) InnerNode {
    int64_t splitValue;
    uint8_t splitDim;
    uint8_t padding[7];
  };

  struct __attribute__((packed)) LeafHeader {
    uint16_t count;
    uint8_t docCodec;
    uint8_t docBits;
    uint8_t bits[NUM_DIMS];
    uint16_t padding;
    uint32_t docBase;
    uint32_t docBytes;
    int64_t base[NUM_DIMS];
    uint32_t dimBytes[NUM_DIMS];
  };

  static_assert(sizeof(FixedHeader) == FIXED_HEADER_SIZE);
  static_assert(sizeof(InnerNode) == 16);
  static_assert(sizeof(LeafHeader) == LEAF_HEADER_SIZE);

  static int64_t dim(const Point& point, uint8_t d) { return d == 0 ? point.min : point.max; }

  static uint32_t numLeftLeaves(uint32_t leaves) {
    if (leaves <= 1) return 0;
    uint32_t floorPower = 1U << (31 - std::countl_zero(leaves));
    return floorPower / 2 + std::min(leaves - floorPower, floorPower / 2);
  }

  // Bytes one dimension's residuals take in a leaf.
  static uint64_t dimByteSize(uint32_t count, uint8_t bits) {
    return bits <= MAX_PACKED_BITS ? LuxirSIMDFor::byteSize(count, bits)
                                   : (uint64_t)count * sizeof(uint64_t);
  }

private:
  OutputStream& out;
  size_t pointsStart;
  uint16_t maxPointsPerLeaf;
  uint32_t totalLeaves = 0;
  std::vector<InnerNode> innerNodes;
  // Tight per-leaf bounds: lo[d][leaf], hi[d][leaf].
  std::vector<int64_t> lo[NUM_DIMS];
  std::vector<int64_t> hi[NUM_DIMS];
  std::vector<uint64_t> leafPointers;

  static void pack(std::span<uint32_t> values, uint8_t bits, std::vector<char>& target) {
    uint64_t bytes = LuxirSIMDFor::byteSize((uint32_t)values.size(), bits);
    target.resize((size_t)bytes);
    uint32_t encodedSize = (uint32_t)target.size();
    IndexCodec::numericCodec.encodeWithMeta(values.data(), (uint32_t)values.size(),
                                            target.data(), encodedSize, 0, bits);
    if (encodedSize != bytes) throw std::runtime_error("RangeBKDWriter: codec byte length mismatch");
  }

  void partition(std::span<Point> points, uint32_t leaves, uint32_t nodeId, uint8_t lastSplitDim) {
    if (leaves == 1) return;
    uint32_t leftLeaves = numLeftLeaves(leaves);
    size_t leftPoints = (size_t)leftLeaves * maxPointsPerLeaf;
    if (leftPoints == 0 || leftPoints >= points.size()) {
      throw std::logic_error("RangeBKDWriter: invalid derived tree partition");
    }

    uint64_t span[NUM_DIMS];
    for (uint8_t d = 0; d < NUM_DIMS; d++) {
      auto [lowest, highest] = std::ranges::minmax(points, {}, [d](const Point& p) { return dim(p, d); });
      span[d] = (uint64_t)dim(highest, d) - (uint64_t)dim(lowest, d);
    }
    // Equal spans alternate away from the parent's split dimension.
    uint8_t splitDim = span[0] == span[1] ? (uint8_t)(lastSplitDim ^ 1) : (span[0] > span[1] ? 0 : 1);
    std::nth_element(points.begin(), points.begin() + (ptrdiff_t)leftPoints, points.end(),
                     [splitDim](const Point& a, const Point& b) {
                       uint8_t other = splitDim ^ 1;
                       if (dim(a, splitDim) != dim(b, splitDim)) return dim(a, splitDim) < dim(b, splitDim);
                       if (dim(a, other) != dim(b, other)) return dim(a, other) < dim(b, other);
                       return a.docid < b.docid;
                     });
    InnerNode& node = innerNodes[nodeId - 1];
    node.splitValue = dim(points[leftPoints], splitDim);
    node.splitDim = splitDim;

    std::span<Point> left = points.first(leftPoints);
    std::span<Point> right = points.subspan(leftPoints);
    TaskGroupRunner runner;
    runner.run(points.size() >= SPAWN_MIN_POINTS, [this, left, leftLeaves, nodeId, splitDim] {
      partition(left, leftLeaves, nodeId * 2, splitDim);
    });
    partition(right, leaves - leftLeaves, nodeId * 2 + 1, splitDim);
    runner.join();
  }

  void writeLeaf(std::span<Point> points, uint32_t leaf, std::vector<uint32_t>& scratch,
                 std::vector<char>& encoded) {
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
      if (a.docid != b.docid) return a.docid < b.docid;
      if (a.min != b.min) return a.min < b.min;
      return a.max < b.max;
    });

    LeafHeader header{};
    header.count = (uint16_t)points.size();
    header.docBase = (uint32_t)points.front().docid;
    bool contiguous = true;
    for (size_t i = 0; i < points.size(); i++) {
      if ((uint64_t)points[i].docid != (uint64_t)header.docBase + i) contiguous = false;
    }
    for (uint8_t d = 0; d < NUM_DIMS; d++) {
      auto [lowest, highest] = std::ranges::minmax(points, {}, [d](const Point& p) { return dim(p, d); });
      lo[d][leaf] = dim(lowest, d);
      hi[d][leaf] = dim(highest, d);
      header.base[d] = lo[d][leaf];
      header.bits[d] = (uint8_t)std::bit_width((uint64_t)hi[d][leaf] - (uint64_t)lo[d][leaf]);
      header.dimBytes[d] = (uint32_t)dimByteSize(header.count, header.bits[d]);
    }

    std::vector<char> docs;
    header.docCodec = contiguous ? DOC_CONTIG : DOC_FOR;
    if (!contiguous) {
      scratch.resize(points.size());
      uint32_t maxResidual = 0;
      for (size_t i = 0; i < points.size(); i++) {
        scratch[i] = (uint32_t)points[i].docid - header.docBase;
        maxResidual = std::max(maxResidual, scratch[i]);
      }
      header.docBits = (uint8_t)std::bit_width(maxResidual);
      pack(scratch, header.docBits, docs);
    }
    header.docBytes = (uint32_t)docs.size();

    leafPointers[leaf] = (uint64_t)(out.size() - pointsStart);
    out.write(&header, sizeof(header));
    out.write(docs.data(), docs.size());
    for (uint8_t d = 0; d < NUM_DIMS; d++) {
      if (header.bits[d] <= MAX_PACKED_BITS) {
        scratch.resize(points.size());
        for (size_t i = 0; i < points.size(); i++) {
          scratch[i] = (uint32_t)((uint64_t)dim(points[i], d) - (uint64_t)header.base[d]);
        }
        pack(scratch, header.bits[d], encoded);
        out.write(encoded.data(), encoded.size());
      } else {
        for (const Point& point : points) {
          uint64_t residual = (uint64_t)dim(point, d) - (uint64_t)header.base[d];
          out.write(&residual, sizeof(residual));
        }
      }
    }
  }

public:
  explicit RangeBKDWriter(OutputStream& out, uint16_t maxPointsPerLeaf = DEFAULT_MAX_POINTS_PER_LEAF)
      : out(out), pointsStart(out.size()), maxPointsPerLeaf(maxPointsPerLeaf) {
    static_assert(std::endian::native == std::endian::little);
    if (maxPointsPerLeaf == 0) throw std::invalid_argument("RangeBKDWriter: maxPointsPerLeaf must be positive");
  }

  // Reorders points. Every point must satisfy min <= max and docid >= 0.
  Data write(std::span<Point> points) {
    if (points.empty()) throw std::invalid_argument("RangeBKDWriter: empty input");
    uint64_t leafCount64 = points.size() / maxPointsPerLeaf + (points.size() % maxPointsPerLeaf != 0);
    if (leafCount64 > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("RangeBKDWriter: too many leaves");
    }
    totalLeaves = (uint32_t)leafCount64;
    innerNodes.assign(totalLeaves - 1, InnerNode{0, 0, {}});
    for (uint8_t d = 0; d < NUM_DIMS; d++) {
      lo[d].resize(totalLeaves);
      hi[d].resize(totalLeaves);
    }
    leafPointers.resize((size_t)totalLeaves + 1);

    partition(points, totalLeaves, 1, 1);
    std::vector<uint32_t> scratch;
    std::vector<char> encoded;
    for (uint32_t leaf = 0; leaf < totalLeaves; leaf++) {
      size_t begin = (size_t)leaf * maxPointsPerLeaf;
      size_t end = std::min(begin + maxPointsPerLeaf, points.size());
      writeLeaf(points.subspan(begin, end - begin), leaf, scratch, encoded);
    }
    uint64_t metaOffset = (uint64_t)(out.size() - pointsStart);
    leafPointers[totalLeaves] = metaOffset;

    FixedHeader header{};
    header.magic = MAGIC;
    header.version = VERSION;
    header.formatKind = FORMAT_KIND;
    header.bytesPerDim = BYTES_PER_DIM;
    header.numDims = NUM_DIMS;
    header.leafCount = totalLeaves;
    header.maxPointsPerLeaf = maxPointsPerLeaf;
    header.pointCount = points.size();
    out.write(&header, sizeof(header));
    out.align(MAX_ALIGN);
    out.write(innerNodes.data(), innerNodes.size() * sizeof(InnerNode));
    for (uint8_t d = 0; d < NUM_DIMS; d++) {
      out.write(lo[d].data(), lo[d].size() * sizeof(int64_t));
      out.write(hi[d].data(), hi[d].size() * sizeof(int64_t));
    }
    out.write(leafPointers.data(), leafPointers.size() * sizeof(uint64_t));
    return {seg_location(out.streamNumber, pointsStart), (int64_t)metaOffset,
            (uint64_t)points.size(), totalLeaves};
  }
};

} // namespace luxir
