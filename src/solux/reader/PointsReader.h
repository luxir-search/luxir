#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "FieldReader.h"
#include "solux/codec/Codec.h"
#include "solux/index/PointsWriter.h"

namespace solux {

class PointsReader {
  const char* points = nullptr;
  const char* fileEnd = nullptr;
  const char* leafMins = nullptr;
  const char* leafMaxes = nullptr;
  const char* leafPointers = nullptr;
  uint64_t pointsFileOffset = 0;
  uint64_t metaOffset = 0;
  uint64_t pointsCount = 0;
  uint32_t leavesCount = 0;
  uint16_t leafSizeMax = 0;
  uint32_t metadataFlags = 0;

  template <class T>
  static T load(const char* ptr) {
    T value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  }

  [[noreturn]] static void invalid(const char* reason) {
    throw std::runtime_error(std::string("PointsReader: ") + reason);
  }

  void init(const InputStream& input, uint64_t pointsOff, int64_t pointsMetaOff) {
    static_assert(std::endian::native == std::endian::little);
    if (pointsMetaOff <= 0) invalid("points index is absent");
    if (pointsOff > (uint64_t)input.size()) invalid("points location is out of bounds");
    if ((uint64_t)pointsMetaOff > (uint64_t)input.size() - pointsOff) invalid("metadata is out of bounds");

    pointsFileOffset = pointsOff;
    metaOffset = (uint64_t)pointsMetaOff;
    points = input.ptr((int64_t)pointsOff);
    fileEnd = input.ptr(input.size());
    const char* meta = points + metaOffset;
    if ((uint64_t)(fileEnd - meta) < PointsWriter::FIXED_HEADER_SIZE) invalid("truncated metadata header");

    if (load<uint32_t>(meta) != PointsWriter::MAGIC) invalid("bad magic");
    if (load<uint16_t>(meta + 4) != PointsWriter::VERSION) invalid("unsupported version");
    if ((uint8_t)meta[6] != PointsWriter::FORMAT_KIND) invalid("unsupported format kind");
    if ((uint8_t)meta[7] != 0x11) invalid("bad metadata reserved byte");
    metadataFlags = load<uint32_t>(meta + 8);
    if (metadataFlags != PointsWriter::FLAG_LEAF_MAX) invalid("unsupported metadata flags");
    leavesCount = load<uint32_t>(meta + 12);
    leafSizeMax = load<uint16_t>(meta + 16);
    if (load<uint16_t>(meta + 18) != 0x1111) invalid("bad metadata reserved word");
    pointsCount = load<uint64_t>(meta + 20);
    if (leavesCount == 0) invalid("leafCount must be positive");
    if (leafSizeMax == 0) invalid("maxPointsPerLeaf must be positive");
    if (pointsCount == 0) invalid("pointCount must be positive");

    uint64_t directoryFileOffset = pointsFileOffset + metaOffset + PointsWriter::FIXED_HEADER_SIZE;
    directoryFileOffset = (directoryFileOffset + 7) & ~(uint64_t)7;
    uint64_t directoryOff = directoryFileOffset - pointsFileOffset;
    uint64_t directoryBytes = (uint64_t)leavesCount * sizeof(int64_t) * 2
                            + ((uint64_t)leavesCount + 1) * sizeof(uint64_t);
    if (directoryOff > (uint64_t)(fileEnd - points)
        || directoryBytes > (uint64_t)(fileEnd - points) - directoryOff) {
      invalid("truncated leaf directory");
    }
    leafMins = points + directoryOff;
    leafMaxes = leafMins + (uint64_t)leavesCount * sizeof(int64_t);
    leafPointers = leafMaxes + (uint64_t)leavesCount * sizeof(int64_t);

    if (leafFP(0) != 0) invalid("first leaf pointer must be zero");
    if (leafFP(leavesCount) != metaOffset) invalid("final leaf pointer does not equal pointsMetaOff");
  }

public:
  struct Point {
    int64_t value;
    int32_t docid;

    bool operator==(const Point&) const = default;
  };

  struct LeafInfo {
    uint16_t count;
    uint8_t docCodec;
    uint8_t docBits;
    uint8_t valueFormat;
    uint32_t docBase;
    uint32_t docBytes;
    int64_t valueMin;
    uint64_t valueGcd;
  };

  PointsReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) {
    if (fieldInfo.pointsMetaOff == 0) invalid("points index is absent");
    InputStream input = postingsReader.getInputStream(fieldInfo.pointsLoc.filenum());
    init(input, fieldInfo.pointsLoc.offset(), fieldInfo.pointsMetaOff);
  }

  PointsReader(const InputStream& input, uint64_t pointsOff, int64_t pointsMetaOff) {
    init(input, pointsOff, pointsMetaOff);
  }

  uint64_t pointCount() const { return pointsCount; }
  uint32_t leafCount() const { return leavesCount; }
  uint16_t maxPointsPerLeaf() const { return leafSizeMax; }
  uint32_t flags() const { return metadataFlags; }

  void validate() const {
    uint64_t countedPoints = 0;
    for (uint32_t i = 0; i < leavesCount; i++) {
      uint64_t start = leafFP(i);
      uint64_t end = leafFP(i + 1);
      if (start >= end) invalid("leaf pointers must be strictly increasing");
      if (end > metaOffset) invalid("leaf overlaps metadata");
      if (end - start < PointsWriter::LEAF_HEADER_SIZE) invalid("truncated leaf header");
      if (i > 0 && leafMin(i) < leafMin(i - 1)) invalid("leafMin is not non-decreasing");
      if (leafMax(i) < leafMin(i)) invalid("leafMax is below leafMin");
      if (i > 0 && leafMin(i) < leafMax(i - 1)) invalid("leaf value ranges are out of order");

      auto info = leafInfo(i);
      if (info.count == 0 || info.count > leafSizeMax) invalid("invalid leaf point count");
      if (info.valueMin != leafMin(i)) invalid("leaf valueMin does not match directory");
      if (info.valueGcd == 0) invalid("valueGcd must be positive");
      if (info.docBase > (uint32_t)std::numeric_limits<int32_t>::max()) invalid("docBase exceeds int32");
      if (info.valueFormat > 64) invalid("invalid value format");
      if ((uint8_t)points[start + 5] != 0x11 || (uint8_t)points[start + 6] != 0x11
          || (uint8_t)points[start + 7] != 0x11) {
        invalid("bad leaf reserved bytes");
      }
      if (info.docCodec == PointsWriter::DOC_CONTIG) {
        if (info.docBits != 0 || info.docBytes != 0) invalid("invalid CONTIG metadata");
        if ((uint64_t)info.docBase + info.count - 1
            > (uint64_t)std::numeric_limits<int32_t>::max()) {
          invalid("CONTIG docids exceed int32");
        }
      } else if (info.docCodec == PointsWriter::DOC_FOR) {
        if (info.docBits > 31) invalid("FOR docBits exceeds 31");
        if (info.docBytes != SoluxSIMDFor::byteSize(info.count, info.docBits)) {
          invalid("invalid FOR byte length");
        }
      } else {
        invalid("reserved docid codec");
      }
      if ((uint64_t)PointsWriter::LEAF_HEADER_SIZE + info.docBytes > end - start) {
        invalid("docid payload exceeds leaf");
      }
      uint64_t valueBytes = end - start - PointsWriter::LEAF_HEADER_SIZE - info.docBytes;
      uint64_t expectedValueBytes = info.valueFormat <= 32
        ? SoluxSIMDFor::byteSize(info.count, info.valueFormat)
        : (uint64_t)info.count * sizeof(int64_t);
      if (valueBytes != expectedValueBytes) invalid("invalid value payload length");
      if (valueAt(i, 0) != leafMin(i) || valueAt(i, info.count - 1) != leafMax(i)) {
        invalid("leaf values do not match directory fences");
      }
      if (std::numeric_limits<uint64_t>::max() - countedPoints < info.count) {
        invalid("pointCount overflow");
      }
      countedPoints += info.count;
    }
    if (countedPoints != pointsCount) invalid("leaf counts do not sum to pointCount");
  }

  int64_t leafMin(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("PointsReader: leaf index");
    return load<int64_t>(leafMins + (uint64_t)leafIndex * sizeof(int64_t));
  }

  int64_t leafMax(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("PointsReader: leaf index");
    return load<int64_t>(leafMaxes + (uint64_t)leafIndex * sizeof(int64_t));
  }

  uint64_t leafFP(uint32_t leafIndex) const {
    if (leafIndex > leavesCount) throw std::out_of_range("PointsReader: leaf pointer index");
    return load<uint64_t>(leafPointers + (uint64_t)leafIndex * sizeof(uint64_t));
  }

  LeafInfo leafInfo(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("PointsReader: leaf index");
    const char* header = points + leafFP(leafIndex);
    return {load<uint16_t>(header), (uint8_t)header[2], (uint8_t)header[3],
            (uint8_t)header[4], load<uint32_t>(header + 8), load<uint32_t>(header + 12),
            load<int64_t>(header + 16), load<uint64_t>(header + 24)};
  }

  int64_t valueAt(uint32_t leafIndex, uint16_t pointIndex) const {
    LeafInfo info = leafInfo(leafIndex);
    if (pointIndex >= info.count) throw std::out_of_range("PointsReader: point index");
    const char* values = points + leafFP(leafIndex) + PointsWriter::LEAF_HEADER_SIZE + info.docBytes;
    if (info.valueFormat <= 32) {
      uint32_t residual = IndexCodec::numericCodec.selectWithMeta(
        values, info.count, pointIndex, 0, info.valueFormat);
      return (int64_t)((uint64_t)residual * info.valueGcd + (uint64_t)info.valueMin);
    }
    return load<int64_t>(values + (uint64_t)pointIndex * sizeof(int64_t));
  }

  std::vector<Point> decodeLeaf(uint32_t leafIndex) const {
    LeafInfo info = leafInfo(leafIndex);
    const char* docPayload = points + leafFP(leafIndex) + PointsWriter::LEAF_HEADER_SIZE;
    const char* valuePayload = docPayload + info.docBytes;
    std::vector<uint32_t> docs(info.count);
    std::vector<uint32_t> values;
    std::vector<Point> decoded(info.count);

    if (info.docCodec == PointsWriter::DOC_CONTIG) {
      for (uint32_t i = 0; i < info.count; i++) docs[i] = info.docBase + i;
    } else {
      uint32_t read = IndexCodec::numericCodec.decodeWithMeta(docPayload, docs.data(),
                                                               info.count, info.docBits);
      if (read != info.docBytes) invalid("FOR decoder byte length mismatch");
      for (auto& doc : docs) {
        uint64_t restored = (uint64_t)doc + info.docBase;
        if (restored > (uint64_t)std::numeric_limits<int32_t>::max()) invalid("decoded docid exceeds int32");
        doc = (uint32_t)restored;
      }
    }

    if (info.valueFormat <= 32) {
      values.resize(info.count);
      uint32_t read = IndexCodec::numericCodec.decodeWithMeta(valuePayload, values.data(),
                                                               info.count, info.valueFormat);
      uint64_t expected = leafFP(leafIndex + 1) - leafFP(leafIndex)
                        - PointsWriter::LEAF_HEADER_SIZE - info.docBytes;
      if (read != expected) invalid("value decoder byte length mismatch");
      for (uint32_t i = 0; i < info.count; i++) {
        decoded[i] = {(int64_t)((uint64_t)values[i] * info.valueGcd + (uint64_t)info.valueMin),
                      (int32_t)docs[i]};
      }
    } else {
      for (uint32_t i = 0; i < info.count; i++) {
        decoded[i] = {load<int64_t>(valuePayload + (uint64_t)i * sizeof(int64_t)),
                      (int32_t)docs[i]};
      }
    }
    return decoded;
  }

  std::vector<Point> readAll() const {
    std::vector<Point> decoded;
    decoded.reserve((size_t)pointsCount);
    for (uint32_t i = 0; i < leavesCount; i++) {
      auto leafPoints = decodeLeaf(i);
      decoded.insert(decoded.end(), leafPoints.begin(), leafPoints.end());
    }
    return decoded;
  }
};

} // namespace solux
