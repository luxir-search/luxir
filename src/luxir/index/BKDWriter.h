// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <oneapi/tbb/parallel_pipeline.h>
#include <oneapi/tbb/task_arena.h>

#include "luxir/index/BKDPointTraits.h"
#include "luxir/index/PointsWriter.h"
#include "luxir/store/OutputStream.h"
#include "luxir/util/thread.h"

namespace luxir {

// Two-dimensional BKD with typed coordinate layouts and payloads.
template <class PointTraits>
class BasicBKDWriter {
public:
  using Scalar = typename PointTraits::Scalar;
  using Unsigned = typename PointTraits::Unsigned;
  using CoordinateCodec = typename PointTraits::CoordinateCodec;
  using InnerNode = typename PointTraits::InnerNode;
  using LeafHeader = typename PointTraits::LeafHeader;

  static constexpr uint32_t MAGIC = PointsWriter::MAGIC;
  static constexpr uint16_t VERSION = 1;
  static constexpr uint8_t FORMAT_KIND = PointTraits::FORMAT_KIND;
  static constexpr uint32_t FLAGS = 0;
  static constexpr uint8_t BYTES_PER_DIM = sizeof(Scalar);
  static constexpr uint8_t NUM_INDEX_DIMS = 2;
  static constexpr uint8_t NUM_DATA_DIMS = 2;
  static constexpr uint16_t DEFAULT_MAX_POINTS_PER_LEAF =
      PointsWriter::DEFAULT_MAX_POINTS_PER_LEAF;
  // pointCount is at offset 24, and the fixed bootstrap ends at offset 32.
  static constexpr uint32_t FIXED_HEADER_SIZE = 32;
  static constexpr uint32_t LEAF_HEADER_SIZE = sizeof(LeafHeader);
  static constexpr uint8_t DOC_CONTIG = PointsWriter::DOC_CONTIG;
  static constexpr uint8_t DOC_FOR = PointsWriter::DOC_FOR;
  static constexpr uint8_t DOC_BITSET = PointsWriter::DOC_BITSET;
  static constexpr uint32_t SPAWN_MIN_POINTS = 32768;

  struct Point {
    Scalar dim0;
    Scalar dim1;
    int32_t docid;

    bool operator==(const Point&) const = default;
  };

  struct Options {
    uint16_t maxPointsPerLeaf = DEFAULT_MAX_POINTS_PER_LEAF;
    bool bitsetDocids = true;
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
    uint8_t reserved;
    uint32_t flags;
    uint32_t leafCount;
    uint16_t maxPointsPerLeaf;
    uint8_t bytesPerDim;
    uint8_t numIndexDims;
    uint8_t numDataDims;
    uint8_t padding[3];
    uint64_t pointCount;
  };

  static_assert(sizeof(FixedHeader) == FIXED_HEADER_SIZE);
  static_assert(offsetof(FixedHeader, pointCount) == 24);

private:
  struct EncodedLeaf {
    uint32_t leaf;
    LeafHeader header;
    std::vector<char> docs;
    std::vector<char> dim0;
    std::vector<char> dim1;
  };

  OutputStream& out;
  size_t pointsStart;
  Options options;
  bool finished = false;
  uint64_t totalPoints = 0;
  uint32_t totalLeaves = 0;
  std::vector<InnerNode> innerNodes;
  std::vector<Scalar> min0;
  std::vector<Scalar> max0;
  std::vector<Scalar> min1;
  std::vector<Scalar> max1;
  std::vector<uint64_t> leafPointers;

  static uint32_t numLeftLeaves(uint32_t leaves) {
    if (leaves <= 1) return 0;
    uint32_t floorPower = 1U << (31 - std::countl_zero(leaves));
    return floorPower / 2
         + std::min(leaves - floorPower, floorPower / 2);
  }

  static std::pair<Unsigned, Unsigned>
  dimSpans(std::span<const Point> points) {
    Scalar min0 = points.front().dim0;
    Scalar max0 = min0;
    Scalar min1 = points.front().dim1;
    Scalar max1 = min1;
    for (const Point& point : points) {
      min0 = std::min(min0, point.dim0);
      max0 = std::max(max0, point.dim0);
      min1 = std::min(min1, point.dim1);
      max1 = std::max(max1, point.dim1);
    }
    return {(Unsigned)max0 - (Unsigned)min0,
            (Unsigned)max1 - (Unsigned)min1};
  }

  static bool lessByDim(const Point& a, const Point& b, uint8_t dim) {
    Scalar aDim = dim == 0 ? a.dim0 : a.dim1;
    Scalar bDim = dim == 0 ? b.dim0 : b.dim1;
    if (aDim != bDim) return aDim < bDim;
    Scalar aOther = dim == 0 ? a.dim1 : a.dim0;
    Scalar bOther = dim == 0 ? b.dim1 : b.dim0;
    if (aOther != bOther) return aOther < bOther;
    return a.docid < b.docid;
  }

  static void encodeDimension(std::span<const Point> points, uint8_t dim,
                              Scalar min, uint8_t bits,
                              std::vector<Unsigned>& scratch,
                              std::vector<char>& target) {
    scratch.resize(points.size());
    for (size_t i = 0; i < points.size(); i++) {
      Scalar value = dim == 0 ? points[i].dim0 : points[i].dim1;
      scratch[i] = (Unsigned)value - (Unsigned)min;
    }
    CoordinateCodec::encode(scratch, bits, target);
  }

  static void encodeDocids(std::span<const Point> points, LeafHeader& header,
                           bool bitsetDocids,
                           std::vector<uint32_t>& scratch,
                           std::vector<uint64_t>& docWords,
                           std::vector<char>& encodedDocs) {
    uint32_t docBase = (uint32_t)points.front().docid;
    for (const Point& point : points) {
      docBase = std::min(docBase, (uint32_t)point.docid);
    }
    header.docBase = docBase;

    bool contiguous = true;
    bool strictlyAscending = true;
    for (size_t i = 0; i < points.size(); i++) {
      if ((uint64_t)points[i].docid != (uint64_t)docBase + i) contiguous = false;
      if (i != 0 && points[i - 1].docid >= points[i].docid) {
        strictlyAscending = false;
      }
    }

    header.docCodec = contiguous ? DOC_CONTIG : DOC_FOR;
    header.docBits = 0;
    encodedDocs.clear();
    if (contiguous) return;

    scratch.resize(points.size());
    uint32_t maxResidual = 0;
    uint32_t maxDocid = docBase;
    for (size_t i = 0; i < points.size(); i++) {
      uint32_t docid = (uint32_t)points[i].docid;
      scratch[i] = docid - docBase;
      maxResidual = std::max(maxResidual, scratch[i]);
      maxDocid = std::max(maxDocid, docid);
    }
    header.docBits = (uint8_t)std::bit_width(maxResidual);
    BKDForCodec::encode(scratch, header.docBits, encodedDocs);

    uint64_t firstWord = (uint64_t)docBase >> 6;
    uint64_t lastWord = (uint64_t)maxDocid >> 6;
    uint64_t bitsetBytes = (lastWord - firstWord + 1) * sizeof(uint64_t);
    if (bitsetDocids && strictlyAscending
        && bitsetBytes < encodedDocs.size()) {
      header.docCodec = DOC_BITSET;
      header.docBits = 0;
      docWords.assign((size_t)(lastWord - firstWord + 1), 0);
      for (const Point& point : points) {
        uint32_t docid = (uint32_t)point.docid;
        size_t word = (size_t)(((uint64_t)docid >> 6) - firstWord);
        docWords[word] |= 1ULL << (docid & 63);
      }
      encodedDocs.resize((size_t)bitsetBytes);
      memcpy(encodedDocs.data(), docWords.data(), encodedDocs.size());
    }
  }

  std::unique_ptr<EncodedLeaf> encodeLeaf(std::span<Point> points,
                                          uint32_t leaf) {
    if (points.empty()) throw std::logic_error("BKDWriter: empty leaf");

    Scalar lo0 = points.front().dim0;
    Scalar hi0 = lo0;
    Scalar lo1 = points.front().dim1;
    Scalar hi1 = lo1;
    for (const Point& point : points) {
      lo0 = std::min(lo0, point.dim0);
      hi0 = std::max(hi0, point.dim0);
      lo1 = std::min(lo1, point.dim1);
      hi1 = std::max(hi1, point.dim1);
    }
    min0[leaf] = lo0;
    max0[leaf] = hi0;
    min1[leaf] = lo1;
    max1[leaf] = hi1;

    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
      if (a.docid != b.docid) return a.docid < b.docid;
      if (a.dim0 != b.dim0) return a.dim0 < b.dim0;
      return a.dim1 < b.dim1;
    });

    auto encoded = std::make_unique<EncodedLeaf>(
        EncodedLeaf{leaf, {}, {}, {}, {}});
    LeafHeader& header = encoded->header;
    header.count = (uint16_t)points.size();
    header.reserved = 0x1111;
    header.min0 = lo0;
    header.min1 = lo1;
    header.reserved2 = 0x11111111;
    std::vector<uint32_t> scratch;
    std::vector<uint64_t> docWords;
    encodeDocids(points, header, options.bitsetDocids, scratch, docWords,
                 encoded->docs);
    header.docBytes = (uint32_t)encoded->docs.size();

    Unsigned range0 = (Unsigned)hi0 - (Unsigned)lo0;
    Unsigned range1 = (Unsigned)hi1 - (Unsigned)lo1;
    header.bits0 = (uint8_t)std::bit_width(range0);
    header.bits1 = (uint8_t)std::bit_width(range1);
    std::vector<Unsigned> residuals;
    encodeDimension(points, 0, lo0, header.bits0, residuals, encoded->dim0);
    encodeDimension(points, 1, lo1, header.bits1, residuals, encoded->dim1);
    header.bytes0 = (uint32_t)encoded->dim0.size();
    return encoded;
  }

  void partition(std::span<Point> points, uint32_t leaves, uint32_t nodeId,
                 uint8_t lastSplitDim) {
    if (leaves == 1) return;

    uint32_t leftLeaves = numLeftLeaves(leaves);
    size_t leftPoints = (size_t)leftLeaves * options.maxPointsPerLeaf;
    if (leftPoints == 0 || leftPoints >= points.size()) {
      throw std::logic_error("BKDWriter: invalid derived tree partition");
    }

    auto [dim0Span, dim1Span] = dimSpans(points);
    // Equal spans alternate away from the parent's split dimension. At the
    // root lastSplitDim is dim1, so an equal root chooses dim0.
    uint8_t splitDim = dim0Span == dim1Span
        ? (uint8_t)(lastSplitDim ^ 1)
        : (dim0Span > dim1Span ? 0 : 1);
    std::nth_element(points.begin(), points.begin() + leftPoints, points.end(),
                     [splitDim](const Point& a, const Point& b) {
                       return lessByDim(a, b, splitDim);
                     });
    Scalar splitValue = splitDim == 0
        ? points[leftPoints].dim0 : points[leftPoints].dim1;
    if (nodeId == 0 || nodeId > innerNodes.size()) {
      throw std::logic_error("BKDWriter: invalid heap node id");
    }
    innerNodes[nodeId - 1] = {splitValue, splitDim, 0x11, 0x1111};

    std::span<Point> left = points.first(leftPoints);
    std::span<Point> right = points.subspan(leftPoints);
    if (points.size() >= SPAWN_MIN_POINTS) {
      TaskGroupRunner runner;
      runner.run(true, [this, left, leftLeaves, nodeId, splitDim] {
        partition(left, leftLeaves, nodeId * 2, splitDim);
      });
      partition(right, leaves - leftLeaves, nodeId * 2 + 1, splitDim);
      runner.join();
    } else {
      partition(left, leftLeaves, nodeId * 2, splitDim);
      partition(right, leaves - leftLeaves, nodeId * 2 + 1, splitDim);
    }
  }

  void emit(std::span<Point> points) {
    uint32_t nextLeaf = 0;
    size_t ntokens = (size_t)4
                   * (size_t)computeWidth(oneapi::tbb::this_task_arena::max_concurrency());
    oneapi::tbb::parallel_pipeline(
        ntokens,
        oneapi::tbb::make_filter<void, uint32_t>(
            oneapi::tbb::filter_mode::serial_in_order,
            [&](oneapi::tbb::flow_control& control) {
              if (nextLeaf == totalLeaves) {
                control.stop();
                return 0U;
              }
              return nextLeaf++;
            })
        & oneapi::tbb::make_filter<uint32_t, std::unique_ptr<EncodedLeaf>>(
            oneapi::tbb::filter_mode::parallel,
            [&](uint32_t leaf) {
              size_t begin = (size_t)leaf * options.maxPointsPerLeaf;
              size_t end = std::min(
                  begin + options.maxPointsPerLeaf, points.size());
              return encodeLeaf(points.subspan(begin, end - begin), leaf);
            })
        & oneapi::tbb::make_filter<std::unique_ptr<EncodedLeaf>, void>(
            oneapi::tbb::filter_mode::serial_in_order,
            [&](std::unique_ptr<EncodedLeaf> encoded) {
              leafPointers[encoded->leaf] =
                  (uint64_t)(out.size() - pointsStart);
              out.write(&encoded->header, sizeof(encoded->header));
              out.write(encoded->docs.data(), encoded->docs.size());
              out.write(encoded->dim0.data(), encoded->dim0.size());
              out.write(encoded->dim1.data(), encoded->dim1.size());
            }));
  }

public:
  explicit BasicBKDWriter(OutputStream& out) : BasicBKDWriter(out, Options{}) {}

  BasicBKDWriter(OutputStream& out, Options options)
      : out(out), pointsStart(out.size()), options(options) {
    static_assert(std::endian::native == std::endian::little);
    if (options.maxPointsPerLeaf == 0) {
      throw std::invalid_argument("BKDWriter: maxPointsPerLeaf must be positive");
    }
  }

  Data write(std::span<Point> points) {
    if (finished) throw std::logic_error("BKDWriter: write called twice");
    if (points.empty()) throw std::invalid_argument("BKDWriter: empty input");
    for (const Point& point : points) {
      if (point.docid < 0) {
        throw std::invalid_argument("BKDWriter: docid must be non-negative");
      }
    }
    totalPoints = points.size();
    uint64_t leafCount64 = totalPoints / options.maxPointsPerLeaf
                         + (totalPoints % options.maxPointsPerLeaf != 0);
    if (leafCount64 > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("BKDWriter: too many leaves");
    }
    totalLeaves = (uint32_t)leafCount64;
    innerNodes.resize(totalLeaves - 1);
    min0.resize(totalLeaves);
    max0.resize(totalLeaves);
    min1.resize(totalLeaves);
    max1.resize(totalLeaves);
    leafPointers.resize((size_t)totalLeaves + 1);

    partition(points, totalLeaves, 1, 1);
    emit(points);
    uint64_t metaOffset = (uint64_t)(out.size() - pointsStart);
    leafPointers[totalLeaves] = metaOffset;

    FixedHeader header{MAGIC, VERSION, FORMAT_KIND, 0x11, FLAGS, totalLeaves,
                       options.maxPointsPerLeaf, BYTES_PER_DIM,
                       NUM_INDEX_DIMS, NUM_DATA_DIMS, {0x11, 0x11, 0x11},
                       totalPoints};
    out.write(&header, sizeof(header));
    out.align(MAX_ALIGN);
    out.write(innerNodes.data(), innerNodes.size() * sizeof(InnerNode));
    out.write(min0.data(), min0.size() * sizeof(Scalar));
    out.write(max0.data(), max0.size() * sizeof(Scalar));
    out.write(min1.data(), min1.size() * sizeof(Scalar));
    out.write(max1.data(), max1.size() * sizeof(Scalar));
    out.write(leafPointers.data(), leafPointers.size() * sizeof(uint64_t));
    finished = true;
    return {seg_location(out.streamNumber, pointsStart), (int64_t)metaOffset,
            totalPoints, totalLeaves};
  }
};

using BKDWriter = BasicBKDWriter<BKDInt32Traits>;

} // namespace luxir
