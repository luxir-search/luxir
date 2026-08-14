#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "FieldReader.h"
#include "luxir/codec/Codec.h"
#include "luxir/index/BKDWriter.h"
#include "luxir/util/geo.h"

namespace luxir {

enum class BKDRelation : uint8_t { OUTSIDE, INSIDE, CROSSES };

struct BKDLongitudeRelation {
  bool inside;
  bool outside;
};

inline BKDLongitudeRelation compareBKDLongitudeInterval(
    int32_t intervalMin, int32_t intervalMax,
    int32_t cellMin, int32_t cellMax) {
  if (intervalMin <= intervalMax) {
    return {intervalMin <= cellMin && cellMax <= intervalMax,
            cellMax < intervalMin || intervalMax < cellMin};
  }
  bool insideUpper = intervalMin <= cellMin;
  bool insideLower = cellMax <= intervalMax;
  bool disjointUpper = cellMax < intervalMin;
  bool disjointLower = intervalMax < cellMin;
  return {insideUpper || insideLower, disjointUpper && disjointLower};
}

// Inclusive encoded-space box. Reversed longitude bounds describe one
// wrapped interval and are evaluated as two arms inside one traversal.
struct BKDBoxRelation {
  int32_t latMin;
  int32_t latMax;
  int32_t lonMin;
  int32_t lonMax;

  BKDBoxRelation(int32_t latMin, int32_t latMax, int32_t lonMin,
                 int32_t lonMax)
      : latMin(latMin), latMax(latMax), lonMin(lonMin), lonMax(lonMax) {
    if (latMax < latMin) {
      throw std::invalid_argument("BKDBoxRelation: latitude bounds are reversed");
    }
  }

  BKDRelation compare(int32_t cellLatMin, int32_t cellLatMax,
                      int32_t cellLonMin, int32_t cellLonMax) const {
    if (cellLatMax < latMin || latMax < cellLatMin) {
      return BKDRelation::OUTSIDE;
    }
    bool latInside = latMin <= cellLatMin && cellLatMax <= latMax;

    BKDLongitudeRelation lon = compareBKDLongitudeInterval(
        lonMin, lonMax, cellLonMin, cellLonMax);
    if (lon.outside) return BKDRelation::OUTSIDE;
    return latInside && lon.inside
        ? BKDRelation::INSIDE : BKDRelation::CROSSES;
  }

  bool matches(int32_t lat, int32_t lon) const {
    return latMin <= lat && lat <= latMax
        && geo::longitudeInRange(lon, lonMin, lonMax);
  }
};

struct BKDDistanceRelation {
  double centerLat;
  double centerLon;
  double centerLatRadians;
  double centerLatCos;
  double radiusMeters;
  double sortKey;
  double axisLatitude;
  int32_t latMin;
  int32_t latMax;
  int32_t lonMin;
  int32_t lonMax;

private:
  static bool within90LonDegrees(double lon, double minLon, double maxLon) {
    if (maxLon <= lon - 180.0) {
      lon -= 360.0;
    } else if (minLon >= lon + 180.0) {
      lon += 360.0;
    }
    return maxLon - lon < 90.0 && lon - minLon < 90.0;
  }

  double pointSortKey(double lat, double lon) const {
    double pointLatRadians = lat * (std::numbers::pi / 180.0);
    double value = (1.0 - std::cos(centerLatRadians - pointLatRadians))
        + centerLatCos * std::cos(pointLatRadians)
            * (1.0 - std::cos((centerLon - lon)
                              * (std::numbers::pi / 180.0)));
    uint64_t bits = std::bit_cast<uint64_t>(value)
                  & UINT64_C(0xfffffffffffffff8);
    return std::bit_cast<double>(bits);
  }

public:
  BKDDistanceRelation(double centerLat, double centerLon,
                      double radiusMeters)
      : centerLat(centerLat), centerLon(centerLon),
        centerLatRadians(centerLat * (std::numbers::pi / 180.0)),
        centerLatCos(std::cos(centerLatRadians)), radiusMeters(radiusMeters),
        sortKey(0.0), axisLatitude(0.0), latMin(0), latMax(0), lonMin(0),
        lonMax(0) {
    geo::checkLatitude(centerLat);
    geo::checkLongitude(centerLon);
    if (!std::isfinite(radiusMeters) || radiusMeters < 0.0) {
      throw std::invalid_argument(
          "BKDDistanceRelation: radius must be finite and non-negative");
    }
    geo::BoundingBox box =
        geo::circleBoundingBox(centerLat, centerLon, radiusMeters);
    latMin = geo::encodeLatitude(box.minLat);
    latMax = geo::encodeLatitude(box.maxLat);
    lonMin = geo::encodeLongitude(box.minLon);
    lonMax = geo::encodeLongitude(box.maxLon);
    sortKey = geo::distanceQuerySortKey(radiusMeters);
    axisLatitude = geo::axisLat(centerLat, radiusMeters);
  }

  BKDRelation compare(int32_t cellLatMin, int32_t cellLatMax,
                      int32_t cellLonMin, int32_t cellLonMax) const {
    BKDLongitudeRelation lon = compareBKDLongitudeInterval(
        lonMin, lonMax, cellLonMin, cellLonMax);
    if (cellLatMax < latMin || latMax < cellLatMin || lon.outside) {
      return BKDRelation::OUTSIDE;
    }

    double minLat = geo::decodeLatitude(cellLatMin);
    double maxLat = geo::decodeLatitude(cellLatMax);
    double minLon = geo::decodeLongitude(cellLonMin);
    double maxLon = geo::decodeLongitude(cellLonMax);
    double minMin = pointSortKey(minLat, minLon);
    double minMax = pointSortKey(minLat, maxLon);
    double maxMin = pointSortKey(maxLat, minLon);
    double maxMax = pointSortKey(maxLat, maxLon);

    if ((centerLon < minLon || centerLon > maxLon)
        && (axisLatitude + geo::AXISLAT_ERROR < minLat
            || axisLatitude - geo::AXISLAT_ERROR > maxLat)
        && minMin > sortKey && minMax > sortKey
        && maxMin > sortKey && maxMax > sortKey) {
      return BKDRelation::OUTSIDE;
    }
    if (within90LonDegrees(centerLon, minLon, maxLon)
        && minMin <= sortKey && minMax <= sortKey
        && maxMin <= sortKey && maxMax <= sortKey) {
      return BKDRelation::INSIDE;
    }
    return BKDRelation::CROSSES;
  }

  bool matches(int32_t lat, int32_t lon) const {
    if (lat < latMin || lat > latMax
        || !geo::longitudeInRange(lon, lonMin, lonMax)) {
      return false;
    }
    return pointSortKey(geo::decodeLatitude(lat), geo::decodeLongitude(lon))
        <= sortKey;
  }
};

class BKDReader {
public:
  using InnerNode = BKDWriter::InnerNode;

  struct LeafInfo {
    uint16_t count;
    uint8_t docCodec;
    uint8_t docBits;
    uint8_t latBits;
    uint8_t lonBits;
    uint32_t docBase;
    uint32_t docBytes;
    int32_t latMin;
    int32_t lonMin;
    uint32_t latBytes;
    uint32_t lonBytes;
  };

  struct Point {
    int32_t lat;
    int32_t lon;
    int32_t docid;

    bool operator==(const Point&) const = default;
  };

  struct CountResult {
    uint64_t exactCount;
  };

  struct EstimateResult {
    uint64_t estimatedCount;
    uint64_t upperBound;
  };

  struct Scratch {
    std::span<uint32_t> docs;
    std::span<uint32_t> lat;
    std::span<uint32_t> lon;
  };

private:
  const char* points = nullptr;
  const char* fileEnd = nullptr;
  const char* innerNodeData = nullptr;
  const char* minLatData = nullptr;
  const char* maxLatData = nullptr;
  const char* minLonData = nullptr;
  const char* maxLonData = nullptr;
  const char* leafPointerData = nullptr;
  uint64_t pointsFileOffset = 0;
  uint64_t metaOffset = 0;
  uint64_t pointsCount = 0;
  uint64_t byteSize = 0;
  uint32_t leavesCount = 0;
  uint16_t leafSizeMax = 0;

  template <class T>
  static T load(const char* ptr) {
    T value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  }

  [[noreturn]] static void invalid(const char* reason) {
    throw std::runtime_error(std::string("BKDReader: ") + reason);
  }

  void init(const InputStream& input, uint64_t pointsOff,
            int64_t pointsMetaOff) {
    static_assert(std::endian::native == std::endian::little);
    if (pointsMetaOff <= 0) invalid("points index is absent");
    if (pointsOff > (uint64_t)input.size()) {
      invalid("points location is out of bounds");
    }
    if ((uint64_t)pointsMetaOff > (uint64_t)input.size() - pointsOff) {
      invalid("metadata is out of bounds");
    }

    pointsFileOffset = pointsOff;
    metaOffset = (uint64_t)pointsMetaOff;
    points = input.ptr((int64_t)pointsOff);
    fileEnd = input.ptr(input.size());
    const char* meta = points + metaOffset;
    if ((uint64_t)(fileEnd - meta) < BKDWriter::FIXED_HEADER_SIZE) {
      invalid("truncated metadata header");
    }
    BKDWriter::FixedHeader header = load<BKDWriter::FixedHeader>(meta);
    if (header.magic != BKDWriter::MAGIC) invalid("bad magic");
    if (header.version != BKDWriter::VERSION) invalid("unsupported version");
    if (header.formatKind != BKDWriter::FORMAT_KIND) {
      invalid("unsupported format kind");
    }
    if (header.reserved != 0x11) invalid("bad metadata reserved byte");
    if (header.flags != BKDWriter::FLAGS) invalid("unsupported metadata flags");
    if (header.maxPointsPerLeaf == 0) {
      invalid("maxPointsPerLeaf must be positive");
    }
    if (header.bytesPerDim != BKDWriter::BYTES_PER_DIM
        || header.numIndexDims != BKDWriter::NUM_INDEX_DIMS
        || header.numDataDims != BKDWriter::NUM_DATA_DIMS) {
      invalid("unsupported dimensions");
    }
    if (header.padding[0] != 0x11 || header.padding[1] != 0x11
        || header.padding[2] != 0x11) {
      invalid("bad metadata padding");
    }
    if (header.leafCount == 0) invalid("leafCount must be positive");
    if (header.pointCount == 0) invalid("pointCount must be positive");

    leavesCount = header.leafCount;
    leafSizeMax = header.maxPointsPerLeaf;
    pointsCount = header.pointCount;
    uint64_t expectedLeaves = pointsCount / leafSizeMax
                            + (pointsCount % leafSizeMax != 0);
    if (expectedLeaves != leavesCount) {
      invalid("leafCount does not match pointCount");
    }

    uint64_t directoryFileOffset = pointsFileOffset + metaOffset
                                 + BKDWriter::FIXED_HEADER_SIZE;
    if (directoryFileOffset < pointsFileOffset) invalid("metadata overflow");
    directoryFileOffset = (directoryFileOffset + 7) & ~(uint64_t)7;
    if (directoryFileOffset < pointsFileOffset) invalid("metadata overflow");
    uint64_t directoryOff = directoryFileOffset - pointsFileOffset;
    uint64_t innerBytes = (uint64_t)(leavesCount - 1)
                        * sizeof(BKDWriter::InnerNode);
    uint64_t boundsBytes = (uint64_t)leavesCount * sizeof(int32_t) * 4;
    uint64_t pointerBytes = ((uint64_t)leavesCount + 1) * sizeof(uint64_t);
    uint64_t directoryBytes = innerBytes + boundsBytes + pointerBytes;
    if (directoryOff > (uint64_t)(fileEnd - points)
        || directoryBytes > (uint64_t)(fileEnd - points) - directoryOff) {
      invalid("truncated metadata arrays");
    }
    byteSize = directoryOff + directoryBytes;
    innerNodeData = points + directoryOff;
    minLatData = innerNodeData + innerBytes;
    maxLatData = minLatData + (uint64_t)leavesCount * sizeof(int32_t);
    minLonData = maxLatData + (uint64_t)leavesCount * sizeof(int32_t);
    maxLonData = minLonData + (uint64_t)leavesCount * sizeof(int32_t);
    leafPointerData = maxLonData + (uint64_t)leavesCount * sizeof(int32_t);

    if (leafFP(0) != 0) invalid("first leaf pointer must be zero");
    if (leafFP(leavesCount) != metaOffset) {
      invalid("final leaf pointer does not equal pointsMetaOff");
    }
  }

  static uint32_t numLeftLeaves(uint32_t leaves) {
    if (leaves <= 1) return 0;
    uint32_t floorPower = 1U << (31 - std::countl_zero(leaves));
    return floorPower / 2
         + std::min(leaves - floorPower, floorPower / 2);
  }

  void validateTree(uint32_t nodeId, uint32_t leaves,
                    int32_t cellLatMin, int32_t cellLatMax,
                    int32_t cellLonMin, int32_t cellLonMax) const {
    if (leaves == 1) return;
    InnerNode node = innerNode(nodeId);
    if (node.splitDim == 0) {
      if (node.splitValue < cellLatMin || node.splitValue > cellLatMax) {
        invalid("latitude split is outside its cell");
      }
    } else {
      if (node.splitValue < cellLonMin || node.splitValue > cellLonMax) {
        invalid("longitude split is outside its cell");
      }
    }
    uint32_t leftLeaves = numLeftLeaves(leaves);
    if (node.splitDim == 0) {
      validateTree(nodeId * 2, leftLeaves, cellLatMin, node.splitValue,
                   cellLonMin, cellLonMax);
      validateTree(nodeId * 2 + 1, leaves - leftLeaves, node.splitValue,
                   cellLatMax, cellLonMin, cellLonMax);
    } else {
      validateTree(nodeId * 2, leftLeaves, cellLatMin, cellLatMax,
                   cellLonMin, node.splitValue);
      validateTree(nodeId * 2 + 1, leaves - leftLeaves, cellLatMin,
                   cellLatMax, node.splitValue, cellLonMax);
    }
  }

  void decodeDocids(uint32_t leafIndex, const LeafInfo& info,
                    std::span<uint32_t> docs) const {
    const char* payload = points + leafFP(leafIndex) + BKDWriter::LEAF_HEADER_SIZE;
    if (info.docCodec == BKDWriter::DOC_CONTIG) {
      for (uint32_t i = 0; i < info.count; i++) docs[i] = info.docBase + i;
    } else if (info.docCodec == BKDWriter::DOC_FOR) {
      uint32_t read = IndexCodec::numericCodec.decodeWithMeta(
          payload, docs.data(), info.count, info.docBits);
      if (read != info.docBytes) invalid("FOR decoder byte length mismatch");
      for (uint32_t i = 0; i < info.count; i++) {
        uint64_t doc = (uint64_t)info.docBase + docs[i];
        if (doc > (uint64_t)std::numeric_limits<int32_t>::max()) {
          invalid("decoded docid exceeds int32");
        }
        docs[i] = (uint32_t)doc;
      }
    } else if (info.docCodec == BKDWriter::DOC_BITSET) {
      uint32_t size = 0;
      uint32_t wordBase = info.docBase >> 6;
      uint32_t wordCount = info.docBytes / sizeof(uint64_t);
      for (uint32_t wordIndex = 0; wordIndex < wordCount; wordIndex++) {
        uint64_t word = load<uint64_t>(
            payload + (uint64_t)wordIndex * sizeof(uint64_t));
        while (word != 0) {
          if (size == info.count) invalid("BITSET popcount exceeds count");
          uint32_t bit = (uint32_t)std::countr_zero(word);
          docs[size++] = (wordBase + wordIndex) * 64 + bit;
          word &= word - 1;
        }
      }
      if (size != info.count) invalid("BITSET popcount does not match count");
    } else {
      invalid("reserved docid codec");
    }
  }

  void decodeCoordinates(uint32_t leafIndex, const LeafInfo& info,
                         std::span<uint32_t> latResiduals,
                         std::span<uint32_t> lonResiduals) const {
    const char* latPayload = points + leafFP(leafIndex)
                           + BKDWriter::LEAF_HEADER_SIZE + info.docBytes;
    const char* lonPayload = latPayload + info.latBytes;
    uint32_t latRead = IndexCodec::numericCodec.decodeWithMeta(
        latPayload, latResiduals.data(), info.count, info.latBits);
    uint32_t lonRead = IndexCodec::numericCodec.decodeWithMeta(
        lonPayload, lonResiduals.data(), info.count, info.lonBits);
    if (latRead != info.latBytes || lonRead != info.lonBytes) {
      invalid("coordinate decoder byte length mismatch");
    }
  }

  template <class EmitDoc, class EmitRun, class EmitWord>
  void emitLeafDocids(uint32_t leafIndex, const LeafInfo& info,
                      std::span<uint32_t> docScratch, EmitDoc& emitDoc,
                      EmitRun& emitRun, EmitWord& emitWord) const {
    const char* payload = points + leafFP(leafIndex) + BKDWriter::LEAF_HEADER_SIZE;
    if (info.docCodec == BKDWriter::DOC_CONTIG) {
      emitRun((int32_t)info.docBase, (int32_t)(info.docBase + info.count));
      return;
    }
    if (info.docCodec == BKDWriter::DOC_BITSET) {
      int32_t wordBase = (int32_t)(info.docBase >> 6);
      uint32_t wordCount = info.docBytes / sizeof(uint64_t);
      for (uint32_t i = 0; i < wordCount; i++) {
        uint64_t word = load<uint64_t>(payload + (uint64_t)i * sizeof(uint64_t));
        if (word != 0) emitWord(wordBase + (int32_t)i, word);
      }
      return;
    }
    decodeDocids(leafIndex, info, docScratch);
    for (uint16_t i = 0; i < info.count; i++) emitDoc((int32_t)docScratch[i]);
  }

public:
  BKDReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) {
    if (fieldInfo.pointsMetaOff == 0) invalid("points index is absent");
    InputStream input = postingsReader.getInputStream(fieldInfo.pointsLoc.filenum());
    init(input, fieldInfo.pointsLoc.offset(), fieldInfo.pointsMetaOff);
  }

  BKDReader(const InputStream& input, uint64_t pointsOff,
            int64_t pointsMetaOff) {
    init(input, pointsOff, pointsMetaOff);
  }

  uint64_t pointCount() const { return pointsCount; }
  uint32_t leafCount() const { return leavesCount; }
  uint16_t maxPointsPerLeaf() const { return leafSizeMax; }
  uint64_t sizeInBytes() const { return byteSize; }

  InnerNode innerNode(uint32_t nodeId) const {
    if (nodeId == 0 || nodeId >= leavesCount) {
      throw std::out_of_range("BKDReader: inner node id");
    }
    InnerNode node = load<InnerNode>(
        innerNodeData + (uint64_t)(nodeId - 1) * sizeof(InnerNode));
    if (node.splitDim > 1) invalid("invalid split dimension");
    if (node.reserved8 != 0x11 || node.reserved16 != 0x1111) {
      invalid("bad inner node reserved bytes");
    }
    return node;
  }

  int32_t leafMinLat(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("BKDReader: leaf index");
    return load<int32_t>(minLatData + (uint64_t)leafIndex * sizeof(int32_t));
  }

  int32_t leafMaxLat(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("BKDReader: leaf index");
    return load<int32_t>(maxLatData + (uint64_t)leafIndex * sizeof(int32_t));
  }

  int32_t leafMinLon(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("BKDReader: leaf index");
    return load<int32_t>(minLonData + (uint64_t)leafIndex * sizeof(int32_t));
  }

  int32_t leafMaxLon(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("BKDReader: leaf index");
    return load<int32_t>(maxLonData + (uint64_t)leafIndex * sizeof(int32_t));
  }

  uint64_t leafFP(uint32_t leafIndex) const {
    if (leafIndex > leavesCount) {
      throw std::out_of_range("BKDReader: leaf pointer index");
    }
    return load<uint64_t>(
        leafPointerData + (uint64_t)leafIndex * sizeof(uint64_t));
  }

  LeafInfo leafInfo(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("BKDReader: leaf index");
    uint64_t start = leafFP(leafIndex);
    uint64_t end = leafFP(leafIndex + 1);
    if (start >= end || end > metaOffset
        || end - start < BKDWriter::LEAF_HEADER_SIZE) {
      invalid("invalid leaf extent");
    }
    BKDWriter::LeafHeader header = load<BKDWriter::LeafHeader>(points + start);
    uint64_t fixedPayload = (uint64_t)BKDWriter::LEAF_HEADER_SIZE
                          + header.docBytes + header.latBytes;
    if (fixedPayload > end - start) invalid("leaf payload lengths exceed extent");
    uint64_t lonBytes = end - start - fixedPayload;
    if (lonBytes > std::numeric_limits<uint32_t>::max()) {
      invalid("longitude payload exceeds uint32");
    }
    return {header.count, header.docCodec, header.docBits, header.latBits,
            header.lonBits, header.docBase, header.docBytes, header.latMin,
            header.lonMin, header.latBytes, (uint32_t)lonBytes};
  }

  void validate() const {
    validateTree(1, leavesCount, INT32_MIN, INT32_MAX, INT32_MIN, INT32_MAX);
    uint64_t countedPoints = 0;
    std::vector<uint32_t> docs(leafSizeMax);
    std::vector<uint32_t> lat(leafSizeMax);
    std::vector<uint32_t> lon(leafSizeMax);
    for (uint32_t i = 0; i < leavesCount; i++) {
      uint64_t start = leafFP(i);
      uint64_t end = leafFP(i + 1);
      if (start >= end) invalid("leaf pointers must be strictly increasing");
      if (end > metaOffset) invalid("leaf overlaps metadata");
      if (leafMinLat(i) > leafMaxLat(i) || leafMinLon(i) > leafMaxLon(i)) {
        invalid("invalid tight leaf bounds");
      }
      LeafInfo info = leafInfo(i);
      if (info.count == 0 || info.count > leafSizeMax) {
        invalid("invalid leaf point count");
      }
      if (i + 1 < leavesCount && info.count != leafSizeMax) {
        invalid("non-final leaf is not full");
      }
      if (info.docBase > (uint32_t)std::numeric_limits<int32_t>::max()) {
        invalid("docBase exceeds int32");
      }
      BKDWriter::LeafHeader raw = load<BKDWriter::LeafHeader>(points + start);
      if (raw.reserved != 0x1111 || raw.reserved2 != 0x11111111) {
        invalid("bad leaf reserved bytes");
      }
      if (info.latMin != leafMinLat(i) || info.lonMin != leafMinLon(i)) {
        invalid("leaf minima do not match directory");
      }
      if (info.latBits > 32 || info.lonBits > 32) {
        invalid("coordinate bit width exceeds 32");
      }
      if (info.docCodec == BKDWriter::DOC_CONTIG) {
        if (info.docBits != 0 || info.docBytes != 0) {
          invalid("invalid CONTIG metadata");
        }
        if ((uint64_t)info.docBase + info.count - 1
            > (uint64_t)std::numeric_limits<int32_t>::max()) {
          invalid("CONTIG docids exceed int32");
        }
      } else if (info.docCodec == BKDWriter::DOC_FOR) {
        if (info.docBits > 31) invalid("FOR docBits exceeds 31");
        if (info.docBytes != LuxirSIMDFor::byteSize(info.count, info.docBits)) {
          invalid("invalid FOR byte length");
        }
      } else if (info.docCodec == BKDWriter::DOC_BITSET) {
        if (info.docBits != 0) invalid("BITSET docBits must be zero");
        if (info.docBytes == 0 || info.docBytes % sizeof(uint64_t) != 0) {
          invalid("invalid BITSET byte length");
        }
        const char* payload = points + start + BKDWriter::LEAF_HEADER_SIZE;
        uint32_t wordCount = info.docBytes / sizeof(uint64_t);
        uint64_t firstWord = load<uint64_t>(payload);
        uint32_t baseBit = info.docBase & 63;
        uint64_t belowBaseMask = baseBit == 0 ? 0 : (1ULL << baseBit) - 1ULL;
        if ((firstWord & belowBaseMask) != 0
            || (firstWord & (1ULL << baseBit)) == 0) {
          invalid("BITSET has bits below docBase or omits docBase");
        }
        uint64_t popcount = 0;
        for (uint32_t wordIndex = 0; wordIndex < wordCount; wordIndex++) {
          popcount += (uint64_t)std::popcount(load<uint64_t>(
              payload + (uint64_t)wordIndex * sizeof(uint64_t)));
        }
        if (popcount != info.count) invalid("BITSET popcount does not match count");
        uint64_t lastWord = load<uint64_t>(
            payload + (uint64_t)(wordCount - 1) * sizeof(uint64_t));
        if (lastWord == 0) invalid("BITSET has trailing zero words");
        uint32_t maxBit = 63 - (uint32_t)std::countl_zero(lastWord);
        uint64_t maxDocid = ((uint64_t)(info.docBase >> 6) + wordCount - 1) * 64
                          + maxBit;
        if (maxDocid > (uint64_t)std::numeric_limits<int32_t>::max()) {
          invalid("BITSET docid exceeds int32");
        }
        uint64_t expectedBytes =
            (((maxDocid >> 6) - ((uint64_t)info.docBase >> 6)) + 1)
            * sizeof(uint64_t);
        if (expectedBytes != info.docBytes) {
          invalid("BITSET byte length does not match docid span");
        }
      } else {
        invalid("reserved docid codec");
      }

      uint64_t expectedLatBytes = LuxirSIMDFor::byteSize(info.count, info.latBits);
      uint64_t expectedLonBytes = LuxirSIMDFor::byteSize(info.count, info.lonBits);
      if (info.latBytes != expectedLatBytes || info.lonBytes != expectedLonBytes) {
        invalid("invalid coordinate payload length");
      }
      decodeDocids(i, info, docs);
      for (uint16_t j = 1; j < info.count; j++) {
        if (docs[j] < docs[j - 1]) invalid("leaf docids are not ascending");
      }
      if (docs[0] != info.docBase) invalid("docBase is not the minimum docid");
      decodeCoordinates(i, info, lat, lon);
      int32_t actualMinLat = INT32_MAX;
      int32_t actualMaxLat = INT32_MIN;
      int32_t actualMinLon = INT32_MAX;
      int32_t actualMaxLon = INT32_MIN;
      for (uint16_t j = 0; j < info.count; j++) {
        int32_t decodedLat = (int32_t)((uint32_t)info.latMin + lat[j]);
        int32_t decodedLon = (int32_t)((uint32_t)info.lonMin + lon[j]);
        actualMinLat = std::min(actualMinLat, decodedLat);
        actualMaxLat = std::max(actualMaxLat, decodedLat);
        actualMinLon = std::min(actualMinLon, decodedLon);
        actualMaxLon = std::max(actualMaxLon, decodedLon);
      }
      if (actualMinLat != leafMinLat(i) || actualMaxLat != leafMaxLat(i)
          || actualMinLon != leafMinLon(i) || actualMaxLon != leafMaxLon(i)) {
        invalid("decoded coordinates do not match tight bounds");
      }
      if (std::numeric_limits<uint64_t>::max() - countedPoints < info.count) {
        invalid("pointCount overflow");
      }
      countedPoints += info.count;
    }
    if (countedPoints != pointsCount) {
      invalid("leaf counts do not sum to pointCount");
    }
  }

  std::vector<Point> decodeLeaf(uint32_t leafIndex) const {
    LeafInfo info = leafInfo(leafIndex);
    std::vector<uint32_t> docs(info.count);
    std::vector<uint32_t> lat(info.count);
    std::vector<uint32_t> lon(info.count);
    decodeDocids(leafIndex, info, docs);
    decodeCoordinates(leafIndex, info, lat, lon);
    std::vector<Point> result(info.count);
    for (uint16_t i = 0; i < info.count; i++) {
      result[i] = {(int32_t)((uint32_t)info.latMin + lat[i]),
                   (int32_t)((uint32_t)info.lonMin + lon[i]),
                   (int32_t)docs[i]};
    }
    return result;
  }

  std::vector<Point> readAll() const {
    std::vector<Point> result;
    result.reserve((size_t)pointsCount);
    for (uint32_t i = 0; i < leavesCount; i++) {
      std::vector<Point> leaf = decodeLeaf(i);
      result.insert(result.end(), leaf.begin(), leaf.end());
    }
    return result;
  }

  template <class Relation, class EmitDoc, class EmitRun, class EmitWord>
  void intersect(Relation&& relation, Scratch scratch, EmitDoc emitDoc,
                 EmitRun emitRun, EmitWord emitWord) const {
    if (scratch.docs.size() < leafSizeMax || scratch.lat.size() < leafSizeMax
        || scratch.lon.size() < leafSizeMax) {
      throw std::invalid_argument("BKDReader: intersect scratch is too small");
    }

    auto visit = [&](auto&& self, uint32_t nodeId, uint32_t leaves,
                     uint32_t firstLeaf, int32_t cellLatMin,
                     int32_t cellLatMax, int32_t cellLonMin,
                     int32_t cellLonMax) -> void {
      BKDRelation relationToCell = relation.compare(
          cellLatMin, cellLatMax, cellLonMin, cellLonMax);
      if (relationToCell == BKDRelation::OUTSIDE) return;
      if (relationToCell == BKDRelation::INSIDE) {
        for (uint32_t leaf = firstLeaf; leaf < firstLeaf + leaves; leaf++) {
          LeafInfo info = leafInfo(leaf);
          emitLeafDocids(leaf, info, scratch.docs, emitDoc, emitRun, emitWord);
        }
        return;
      }
      if (leaves != 1) {
        InnerNode node = innerNode(nodeId);
        uint32_t leftLeaves = numLeftLeaves(leaves);
        if (node.splitDim == 0) {
          self(self, nodeId * 2, leftLeaves, firstLeaf, cellLatMin,
               node.splitValue, cellLonMin, cellLonMax);
          self(self, nodeId * 2 + 1, leaves - leftLeaves,
               firstLeaf + leftLeaves, node.splitValue, cellLatMax,
               cellLonMin, cellLonMax);
        } else {
          self(self, nodeId * 2, leftLeaves, firstLeaf, cellLatMin,
               cellLatMax, cellLonMin, node.splitValue);
          self(self, nodeId * 2 + 1, leaves - leftLeaves,
               firstLeaf + leftLeaves, cellLatMin, cellLatMax,
               node.splitValue, cellLonMax);
        }
        return;
      }

      LeafInfo info = leafInfo(firstLeaf);
      BKDRelation tight = relation.compare(
          leafMinLat(firstLeaf), leafMaxLat(firstLeaf),
          leafMinLon(firstLeaf), leafMaxLon(firstLeaf));
      if (tight == BKDRelation::OUTSIDE) return;
      if (tight == BKDRelation::INSIDE) {
        emitLeafDocids(firstLeaf, info, scratch.docs, emitDoc, emitRun, emitWord);
        return;
      }
      decodeDocids(firstLeaf, info, scratch.docs);
      decodeCoordinates(firstLeaf, info, scratch.lat, scratch.lon);
      for (uint16_t i = 0; i < info.count; i++) {
        int32_t decodedLat = (int32_t)((uint32_t)info.latMin + scratch.lat[i]);
        int32_t decodedLon = (int32_t)((uint32_t)info.lonMin + scratch.lon[i]);
        if (relation.matches(decodedLat, decodedLon)) {
          emitDoc((int32_t)scratch.docs[i]);
        }
      }
    };
    visit(visit, 1, leavesCount, 0, INT32_MIN, INT32_MAX,
          INT32_MIN, INT32_MAX);
  }

  template <class Relation>
  EstimateResult estimateIntersect(Relation&& relation) const {
    uint64_t estimated = 0;
    uint64_t upper = 0;
    auto addLeaves = [&](uint32_t firstLeaf, uint32_t leaves) {
      for (uint32_t leaf = firstLeaf; leaf < firstLeaf + leaves; leaf++) {
        uint64_t count = leafInfo(leaf).count;
        estimated += count;
        upper += count;
      }
    };
    auto visit = [&](auto&& self, uint32_t nodeId, uint32_t leaves,
                     uint32_t firstLeaf, int32_t cellLatMin,
                     int32_t cellLatMax, int32_t cellLonMin,
                     int32_t cellLonMax) -> void {
      BKDRelation relationToCell = relation.compare(
          cellLatMin, cellLatMax, cellLonMin, cellLonMax);
      if (relationToCell == BKDRelation::OUTSIDE) return;
      if (relationToCell == BKDRelation::INSIDE) {
        addLeaves(firstLeaf, leaves);
        return;
      }
      if (leaves != 1) {
        InnerNode node = innerNode(nodeId);
        uint32_t leftLeaves = numLeftLeaves(leaves);
        if (node.splitDim == 0) {
          self(self, nodeId * 2, leftLeaves, firstLeaf, cellLatMin,
               node.splitValue, cellLonMin, cellLonMax);
          self(self, nodeId * 2 + 1, leaves - leftLeaves,
               firstLeaf + leftLeaves, node.splitValue, cellLatMax,
               cellLonMin, cellLonMax);
        } else {
          self(self, nodeId * 2, leftLeaves, firstLeaf, cellLatMin,
               cellLatMax, cellLonMin, node.splitValue);
          self(self, nodeId * 2 + 1, leaves - leftLeaves,
               firstLeaf + leftLeaves, cellLatMin, cellLatMax,
               node.splitValue, cellLonMax);
        }
        return;
      }

      LeafInfo info = leafInfo(firstLeaf);
      BKDRelation tight = relation.compare(
          leafMinLat(firstLeaf), leafMaxLat(firstLeaf),
          leafMinLon(firstLeaf), leafMaxLon(firstLeaf));
      if (tight == BKDRelation::INSIDE) {
        estimated += info.count;
        upper += info.count;
      } else if (tight == BKDRelation::CROSSES) {
        estimated += ((uint64_t)info.count + 1) / 2;
        upper += info.count;
      }
    };
    visit(visit, 1, leavesCount, 0, INT32_MIN, INT32_MAX,
          INT32_MIN, INT32_MAX);
    return {estimated, upper};
  }

  template <class Relation>
  CountResult countIntersect(Relation&& relation, Scratch scratch) const {
    if (scratch.lat.size() < leafSizeMax || scratch.lon.size() < leafSizeMax) {
      throw std::invalid_argument("BKDReader: count scratch is too small");
    }
    uint64_t count = 0;
    auto visit = [&](auto&& self, uint32_t nodeId, uint32_t leaves,
                     uint32_t firstLeaf, int32_t cellLatMin,
                     int32_t cellLatMax, int32_t cellLonMin,
                     int32_t cellLonMax) -> void {
      BKDRelation relationToCell = relation.compare(
          cellLatMin, cellLatMax, cellLonMin, cellLonMax);
      if (relationToCell == BKDRelation::OUTSIDE) return;
      if (relationToCell == BKDRelation::INSIDE) {
        for (uint32_t leaf = firstLeaf; leaf < firstLeaf + leaves; leaf++) {
          count += leafInfo(leaf).count;
        }
        return;
      }
      if (leaves != 1) {
        InnerNode node = innerNode(nodeId);
        uint32_t leftLeaves = numLeftLeaves(leaves);
        if (node.splitDim == 0) {
          self(self, nodeId * 2, leftLeaves, firstLeaf, cellLatMin,
               node.splitValue, cellLonMin, cellLonMax);
          self(self, nodeId * 2 + 1, leaves - leftLeaves,
               firstLeaf + leftLeaves, node.splitValue, cellLatMax,
               cellLonMin, cellLonMax);
        } else {
          self(self, nodeId * 2, leftLeaves, firstLeaf, cellLatMin,
               cellLatMax, cellLonMin, node.splitValue);
          self(self, nodeId * 2 + 1, leaves - leftLeaves,
               firstLeaf + leftLeaves, cellLatMin, cellLatMax,
               node.splitValue, cellLonMax);
        }
        return;
      }
      LeafInfo info = leafInfo(firstLeaf);
      BKDRelation tight = relation.compare(
          leafMinLat(firstLeaf), leafMaxLat(firstLeaf),
          leafMinLon(firstLeaf), leafMaxLon(firstLeaf));
      if (tight == BKDRelation::OUTSIDE) return;
      if (tight == BKDRelation::INSIDE) {
        count += info.count;
        return;
      }
      decodeCoordinates(firstLeaf, info, scratch.lat, scratch.lon);
      for (uint16_t i = 0; i < info.count; i++) {
        int32_t decodedLat = (int32_t)((uint32_t)info.latMin + scratch.lat[i]);
        int32_t decodedLon = (int32_t)((uint32_t)info.lonMin + scratch.lon[i]);
        if (relation.matches(decodedLat, decodedLon)) count++;
      }
    };
    visit(visit, 1, leavesCount, 0, INT32_MIN, INT32_MAX,
          INT32_MIN, INT32_MAX);
    return {count};
  }
};

} // namespace luxir
