#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "solux/codec/Codec.h"
#include "solux/index/PointsWriter.h"
#include "solux/store/OutputStream.h"

namespace solux {

// Typed two-dimensional int32 BKD. A future dimensionality or scalar width is
// a distinct format specialization, even though the bootstrap records both.
class BKDWriter {
public:
  static constexpr uint32_t MAGIC = PointsWriter::MAGIC;
  static constexpr uint16_t VERSION = 1;
  static constexpr uint8_t FORMAT_KIND = 2;
  static constexpr uint32_t FLAGS = 0;
  static constexpr uint8_t BYTES_PER_DIM = 4;
  static constexpr uint8_t NUM_INDEX_DIMS = 2;
  static constexpr uint8_t NUM_DATA_DIMS = 2;
  static constexpr uint16_t DEFAULT_MAX_POINTS_PER_LEAF =
      PointsWriter::DEFAULT_MAX_POINTS_PER_LEAF;
  // pointCount is at offset 24, and the fixed bootstrap ends at offset 32.
  static constexpr uint32_t FIXED_HEADER_SIZE = 32;
  static constexpr uint32_t LEAF_HEADER_SIZE = 32;
  static constexpr uint8_t DOC_CONTIG = PointsWriter::DOC_CONTIG;
  static constexpr uint8_t DOC_FOR = PointsWriter::DOC_FOR;
  static constexpr uint8_t DOC_BITSET = PointsWriter::DOC_BITSET;

  struct Point {
    int32_t lat;
    int32_t lon;
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

  // Logical nodes are 1-indexed in heap order. Stored entry nodeId - 1 holds
  // nodeId, so there is no unused on-disk entry zero.
  struct __attribute__((packed)) InnerNode {
    int32_t splitValue;
    uint8_t splitDim;
    uint8_t reserved8;
    uint16_t reserved16;
  };

  struct __attribute__((packed)) LeafHeader {
    uint16_t count;
    uint8_t docCodec;
    uint8_t docBits;
    uint8_t latBits;
    uint8_t lonBits;
    uint16_t reserved;
    uint32_t docBase;
    uint32_t docBytes;
    int32_t latMin;
    int32_t lonMin;
    uint32_t latBytes;
    uint32_t reserved2;
  };

  static_assert(sizeof(FixedHeader) == FIXED_HEADER_SIZE);
  static_assert(offsetof(FixedHeader, pointCount) == 24);
  static_assert(sizeof(InnerNode) == 8);
  static_assert(offsetof(InnerNode, splitDim) == 4);
  static_assert(sizeof(LeafHeader) == LEAF_HEADER_SIZE);
  static_assert(offsetof(LeafHeader, docBase) == 8);
  static_assert(offsetof(LeafHeader, latMin) == 16);

private:
  OutputStream& out;
  size_t pointsStart;
  Options options;
  bool finished = false;
  uint64_t totalPoints = 0;
  uint32_t totalLeaves = 0;
  std::vector<InnerNode> innerNodes;
  std::vector<int32_t> minLat;
  std::vector<int32_t> maxLat;
  std::vector<int32_t> minLon;
  std::vector<int32_t> maxLon;
  std::vector<uint64_t> leafPointers;
  std::vector<uint32_t> scratch;
  std::vector<char> encodedDocs;
  std::vector<char> encodedLat;
  std::vector<char> encodedLon;
  std::vector<uint64_t> docWords;

  static uint32_t numLeftLeaves(uint32_t leaves) {
    if (leaves <= 1) return 0;
    uint32_t floorPower = 1U << (31 - std::countl_zero(leaves));
    return floorPower / 2
         + std::min(leaves - floorPower, floorPower / 2);
  }

  static uint32_t dimSpan(std::span<const Point> points, uint8_t dim) {
    int32_t min = dim == 0 ? points.front().lat : points.front().lon;
    int32_t max = min;
    for (const Point& point : points) {
      int32_t value = dim == 0 ? point.lat : point.lon;
      min = std::min(min, value);
      max = std::max(max, value);
    }
    return (uint32_t)max - (uint32_t)min;
  }

  static bool lessByDim(const Point& a, const Point& b, uint8_t dim) {
    int32_t aDim = dim == 0 ? a.lat : a.lon;
    int32_t bDim = dim == 0 ? b.lat : b.lon;
    if (aDim != bDim) return aDim < bDim;
    int32_t aOther = dim == 0 ? a.lon : a.lat;
    int32_t bOther = dim == 0 ? b.lon : b.lat;
    if (aOther != bOther) return aOther < bOther;
    return a.docid < b.docid;
  }

  void encode(std::span<uint32_t> values, uint8_t bits,
              std::vector<char>& target) {
    uint64_t bytes = SoluxSIMDFor::byteSize((uint32_t)values.size(), bits);
    if (bytes > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("BKDWriter: encoded leaf exceeds uint32");
    }
    target.resize((size_t)bytes);
    uint32_t encodedSize = (uint32_t)target.size();
    IndexCodec::numericCodec.encodeWithMeta(
        values.data(), (uint32_t)values.size(), target.data(), encodedSize, 0,
        bits);
    if (encodedSize != bytes) {
      throw std::runtime_error("BKDWriter: codec byte length mismatch");
    }
  }

  void encodeDimension(std::span<const Point> points, uint8_t dim,
                       int32_t min, uint8_t bits, std::vector<char>& target) {
    scratch.resize(points.size());
    for (size_t i = 0; i < points.size(); i++) {
      int32_t value = dim == 0 ? points[i].lat : points[i].lon;
      scratch[i] = (uint32_t)value - (uint32_t)min;
    }
    encode(scratch, bits, target);
  }

  void encodeDocids(std::span<const Point> points, LeafHeader& header) {
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
    encode(scratch, header.docBits, encodedDocs);

    uint64_t firstWord = (uint64_t)docBase >> 6;
    uint64_t lastWord = (uint64_t)maxDocid >> 6;
    uint64_t bitsetBytes = (lastWord - firstWord + 1) * sizeof(uint64_t);
    if (options.bitsetDocids && strictlyAscending
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

  void flushLeaf(std::span<Point> points) {
    if (points.empty()) throw std::logic_error("BKDWriter: empty leaf");
    leafPointers.push_back((uint64_t)(out.size() - pointsStart));

    int32_t loLat = points.front().lat;
    int32_t hiLat = loLat;
    int32_t loLon = points.front().lon;
    int32_t hiLon = loLon;
    for (const Point& point : points) {
      loLat = std::min(loLat, point.lat);
      hiLat = std::max(hiLat, point.lat);
      loLon = std::min(loLon, point.lon);
      hiLon = std::max(hiLon, point.lon);
    }
    minLat.push_back(loLat);
    maxLat.push_back(hiLat);
    minLon.push_back(loLon);
    maxLon.push_back(hiLon);

    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
      if (a.docid != b.docid) return a.docid < b.docid;
      if (a.lat != b.lat) return a.lat < b.lat;
      return a.lon < b.lon;
    });

    LeafHeader header{};
    header.count = (uint16_t)points.size();
    header.reserved = 0x1111;
    header.latMin = loLat;
    header.lonMin = loLon;
    header.reserved2 = 0x11111111;
    encodeDocids(points, header);
    header.docBytes = (uint32_t)encodedDocs.size();

    uint32_t latRange = (uint32_t)hiLat - (uint32_t)loLat;
    uint32_t lonRange = (uint32_t)hiLon - (uint32_t)loLon;
    header.latBits = (uint8_t)std::bit_width(latRange);
    header.lonBits = (uint8_t)std::bit_width(lonRange);
    encodeDimension(points, 0, loLat, header.latBits, encodedLat);
    encodeDimension(points, 1, loLon, header.lonBits, encodedLon);
    header.latBytes = (uint32_t)encodedLat.size();

    out.write(&header, sizeof(header));
    out.write(encodedDocs.data(), encodedDocs.size());
    out.write(encodedLat.data(), encodedLat.size());
    out.write(encodedLon.data(), encodedLon.size());
  }

  void build(std::span<Point> points, uint32_t leaves, uint32_t nodeId,
             uint8_t lastSplitDim) {
    if (leaves == 1) {
      flushLeaf(points);
      return;
    }

    uint32_t leftLeaves = numLeftLeaves(leaves);
    size_t leftPoints = (size_t)leftLeaves * options.maxPointsPerLeaf;
    if (leftPoints == 0 || leftPoints >= points.size()) {
      throw std::logic_error("BKDWriter: invalid derived tree partition");
    }

    uint32_t latSpan = dimSpan(points, 0);
    uint32_t lonSpan = dimSpan(points, 1);
    // Equal spans alternate away from the parent's split dimension. At the
    // root lastSplitDim is lon, so an equal root chooses lat.
    uint8_t splitDim = latSpan == lonSpan
        ? (uint8_t)(lastSplitDim ^ 1)
        : (latSpan > lonSpan ? 0 : 1);
    std::nth_element(points.begin(), points.begin() + leftPoints, points.end(),
                     [splitDim](const Point& a, const Point& b) {
                       return lessByDim(a, b, splitDim);
                     });
    int32_t splitValue = splitDim == 0
        ? points[leftPoints].lat : points[leftPoints].lon;
    if (nodeId == 0 || nodeId > innerNodes.size()) {
      throw std::logic_error("BKDWriter: invalid heap node id");
    }
    innerNodes[nodeId - 1] = {splitValue, splitDim, 0x11, 0x1111};

    build(points.first(leftPoints), leftLeaves, nodeId * 2, splitDim);
    build(points.subspan(leftPoints), leaves - leftLeaves, nodeId * 2 + 1,
          splitDim);
  }

public:
  explicit BKDWriter(OutputStream& out) : BKDWriter(out, Options{}) {}

  BKDWriter(OutputStream& out, Options options)
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
    minLat.reserve(totalLeaves);
    maxLat.reserve(totalLeaves);
    minLon.reserve(totalLeaves);
    maxLon.reserve(totalLeaves);
    leafPointers.reserve((size_t)totalLeaves + 1);

    build(points, totalLeaves, 1, 1);
    uint64_t metaOffset = (uint64_t)(out.size() - pointsStart);
    leafPointers.push_back(metaOffset);

    FixedHeader header{MAGIC, VERSION, FORMAT_KIND, 0x11, FLAGS, totalLeaves,
                       options.maxPointsPerLeaf, BYTES_PER_DIM,
                       NUM_INDEX_DIMS, NUM_DATA_DIMS, {0x11, 0x11, 0x11},
                       totalPoints};
    out.write(&header, sizeof(header));
    out.align(8);
    out.write(innerNodes.data(), innerNodes.size() * sizeof(InnerNode));
    out.write(minLat.data(), minLat.size() * sizeof(int32_t));
    out.write(maxLat.data(), maxLat.size() * sizeof(int32_t));
    out.write(minLon.data(), minLon.size() * sizeof(int32_t));
    out.write(maxLon.data(), maxLon.size() * sizeof(int32_t));
    out.write(leafPointers.data(), leafPointers.size() * sizeof(uint64_t));
    finished = true;
    return {seg_location(out.streamNumber, pointsStart), (int64_t)metaOffset,
            totalPoints, totalLeaves};
  }
};

} // namespace solux
