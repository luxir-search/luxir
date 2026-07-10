#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
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
  struct FenceRange {
    uint32_t firstLeaf = 0;
    uint32_t lastLeaf = 0;
    uint64_t firstOrdinal = 0;
    uint64_t endOrdinal = 0;
    bool empty = true;
  };

  struct LeafValueBounds {
    uint16_t lower = 0;
    uint16_t upper = 0;
  };

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

  uint64_t leafOrdinalStart(uint32_t leafIndex) const {
    if (leafIndex >= leavesCount) throw std::out_of_range("PointsReader: leaf index");
    return (uint64_t)leafIndex * leafSizeMax;
  }

  uint64_t leafOrdinalEnd(uint32_t leafIndex) const {
    return std::min(pointsCount, leafOrdinalStart(leafIndex) + leafSizeMax);
  }

  FenceRange fenceRange(int64_t lo, int64_t hi) const {
    FenceRange range;
    if (hi < lo) return range;

    uint32_t leftLo = 0;
    uint32_t leftHi = leavesCount;
    while (leftLo < leftHi) {
      uint32_t mid = leftLo + (leftHi - leftLo) / 2;
      if (leafMax(mid) < lo) leftLo = mid + 1;
      else leftHi = mid;
    }

    uint32_t rightLo = 0;
    uint32_t rightHi = leavesCount;
    while (rightLo < rightHi) {
      uint32_t mid = rightLo + (rightHi - rightLo) / 2;
      if (leafMin(mid) <= hi) rightLo = mid + 1;
      else rightHi = mid;
    }
    if (leftLo >= rightLo) return range;

    range.firstLeaf = leftLo;
    range.lastLeaf = rightLo - 1;
    range.firstOrdinal = leafOrdinalStart(range.firstLeaf);
    range.endOrdinal = leafOrdinalEnd(range.lastLeaf);
    range.empty = false;
    return range;
  }

  LeafValueBounds valueBounds(uint32_t leafIndex, int64_t lo, int64_t hi,
                              std::span<uint32_t> residualScratch,
                              std::span<int64_t> rawScratch) const {
    LeafInfo info = leafInfo(leafIndex);
    LeafValueBounds bounds;
    int64_t min = leafMin(leafIndex);
    int64_t max = leafMax(leafIndex);
    if (hi < min || max < lo) return bounds;
    if (lo <= min && max <= hi) return {0, info.count};

    const char* payload = points + leafFP(leafIndex) + PointsWriter::LEAF_HEADER_SIZE
                        + info.docBytes;
    if (info.valueFormat <= 32) {
      if (residualScratch.size() < info.count) {
        throw std::invalid_argument("PointsReader: residual scratch is too small");
      }
      IndexCodec::numericCodec.decodeWithMeta(payload, residualScratch.data(),
                                               info.count, info.valueFormat);
      uint64_t lower = 0;
      if (lo > min) {
        uint64_t delta = (uint64_t)lo - (uint64_t)min;
        lower = delta / info.valueGcd + (delta % info.valueGcd != 0);
      }
      uint64_t upper = ((uint64_t)max - (uint64_t)min) / info.valueGcd;
      if (hi < max) {
        upper = ((uint64_t)hi - (uint64_t)min) / info.valueGcd;
      }
      if (lower > UINT32_MAX) return bounds;
      uint32_t lowerResidual = (uint32_t)lower;
      uint32_t upperResidual = upper > UINT32_MAX ? UINT32_MAX : (uint32_t)upper;
      auto values = residualScratch.first(info.count);
      bounds.lower = (uint16_t)(std::lower_bound(values.begin(), values.end(),
                                                  lowerResidual) - values.begin());
      bounds.upper = (uint16_t)(std::upper_bound(values.begin(), values.end(),
                                                  upperResidual) - values.begin());
      return bounds;
    }

    if (rawScratch.size() < info.count) {
      throw std::invalid_argument("PointsReader: raw scratch is too small");
    }
    memcpy(rawScratch.data(), payload, (size_t)info.count * sizeof(int64_t));
    auto values = rawScratch.first(info.count);
    bounds.lower = (uint16_t)(std::lower_bound(values.begin(), values.end(), lo)
                               - values.begin());
    bounds.upper = (uint16_t)(std::upper_bound(values.begin(), values.end(), hi)
                               - values.begin());
    return bounds;
  }

  template <class EmitDoc, class EmitRun>
  void emitDocids(uint32_t leafIndex, uint16_t begin, uint16_t end,
                  std::span<uint32_t> docScratch, EmitDoc emitDoc,
                  EmitRun emitRun) const {
    LeafInfo info = leafInfo(leafIndex);
    if (begin > end || end > info.count) {
      throw std::out_of_range("PointsReader: leaf point range");
    }
    if (begin == end) return;
    if (info.docCodec == PointsWriter::DOC_CONTIG) {
      emitRun((int32_t)(info.docBase + begin), (int32_t)(info.docBase + end));
      return;
    }
    if (info.docCodec != PointsWriter::DOC_FOR) invalid("reserved docid codec");
    if (docScratch.size() < info.count) {
      throw std::invalid_argument("PointsReader: docid scratch is too small");
    }
    const char* payload = points + leafFP(leafIndex) + PointsWriter::LEAF_HEADER_SIZE;
    IndexCodec::numericCodec.decodeWithMeta(payload, docScratch.data(), info.count,
                                             info.docBits);
    for (uint16_t i = begin; i < end; i++) {
      uint64_t doc = (uint64_t)info.docBase + docScratch[i];
      if (doc > (uint64_t)std::numeric_limits<int32_t>::max()) {
        invalid("decoded docid exceeds int32");
      }
      emitDoc((int32_t)doc);
    }
  }

  template <class EmitDoc, class EmitRun>
  void emitOrdinalRange(uint64_t begin, uint64_t end,
                        std::span<uint32_t> docScratch, EmitDoc emitDoc,
                        EmitRun emitRun) const {
    if (begin > end || end > pointsCount) {
      throw std::out_of_range("PointsReader: point ordinal range");
    }
    while (begin < end) {
      uint32_t leafIndex = (uint32_t)(begin / leafSizeMax);
      uint64_t leafStart = leafOrdinalStart(leafIndex);
      uint64_t leafEnd = leafOrdinalEnd(leafIndex);
      uint64_t partEnd = std::min(end, leafEnd);
      emitDocids(leafIndex, (uint16_t)(begin - leafStart),
                 (uint16_t)(partEnd - leafStart), docScratch, emitDoc, emitRun);
      begin = partEnd;
    }
  }

  uint16_t decodeLeafInto(uint32_t leafIndex, std::span<int64_t> values,
                          std::span<uint32_t> docids,
                          std::span<uint32_t> residualScratch) const {
    LeafInfo info = leafInfo(leafIndex);
    if (values.size() < info.count || docids.size() < info.count
        || residualScratch.size() < info.count) {
      throw std::invalid_argument("PointsReader: leaf decode scratch is too small");
    }

    const char* docPayload = points + leafFP(leafIndex) + PointsWriter::LEAF_HEADER_SIZE;
    const char* valuePayload = docPayload + info.docBytes;
    if (info.docCodec == PointsWriter::DOC_CONTIG) {
      for (uint32_t i = 0; i < info.count; i++) docids[i] = info.docBase + i;
    } else if (info.docCodec == PointsWriter::DOC_FOR) {
      IndexCodec::numericCodec.decodeWithMeta(docPayload, docids.data(), info.count,
                                               info.docBits);
      for (uint32_t i = 0; i < info.count; i++) {
        uint64_t doc = (uint64_t)info.docBase + docids[i];
        if (doc > (uint64_t)std::numeric_limits<int32_t>::max()) {
          invalid("decoded docid exceeds int32");
        }
        docids[i] = (uint32_t)doc;
      }
    } else {
      invalid("reserved docid codec");
    }

    if (info.valueFormat <= 32) {
      IndexCodec::numericCodec.decodeWithMeta(valuePayload, residualScratch.data(),
                                               info.count, info.valueFormat);
      for (uint32_t i = 0; i < info.count; i++) {
        values[i] = (int64_t)((uint64_t)residualScratch[i] * info.valueGcd
                              + (uint64_t)info.valueMin);
      }
    } else {
      memcpy(values.data(), valuePayload, (size_t)info.count * sizeof(int64_t));
    }
    return info.count;
  }

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
      if (i + 1 < leavesCount && info.count != leafSizeMax) {
        invalid("non-final leaf is not full");
      }
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
