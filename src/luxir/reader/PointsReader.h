// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "PostingsReader.h"
#include "luxir/codec/ForCodec.h"
#include "luxir/index/PointsWriter.h"
#include "luxir/util/screaming.h"

namespace luxir {

// Reads one-dimensional point leaves and their value fences.
class PointsReader {
  const char* points = nullptr;  // Start of the encoded leaves.
  const char* leafMins = nullptr;  // Least value per leaf.
  const char* leafMaxes = nullptr;  // Greatest value per leaf.
  const char* leafPointers = nullptr;  // Leaf offsets relative to the first leaf.
  const char* leafEncodings = nullptr;  // Per-leaf encoding summaries.
  uint64_t metaOffset = 0;  // Byte length of the leaves.
  uint64_t pointsCount = 0;
  uint64_t byteSize = 0;  // Byte length of leaves and directory.
  uint32_t leavesCount = 0;
  uint16_t leafSizeMax = 0;  // Capacity of a full leaf.
  uint32_t metadataFlags = 0;
  SegmentReadContext context;  // Source of the point data for corruption reports.

  template <class T>
  static T load(const char* ptr) {
    T value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  }

  [[noreturn]] void invalid(const char* reason) const {
    context.invalid(reason, "PointsReader");
  }

  void init(std::span<const char> leaves, std::span<const char> directory,
            uint64_t directoryOffset) {
    static_assert(std::endian::native == std::endian::little);
    if (leaves.empty()) invalid("points index is absent");
    if (directory.size() < PointsWriter::FIXED_HEADER_SIZE) invalid("truncated metadata header");
    points = leaves.data();
    metaOffset = leaves.size();
    const char* meta = directory.data();
    auto header = load<PointsWriter::FixedHeader>(meta);
    if (header.magic != PointsWriter::MAGIC) invalid("bad directory magic");
    if (header.version != PointsWriter::VERSION) invalid("unsupported directory version");
    if (header.formatKind != PointsWriter::FORMAT_KIND) invalid("unsupported directory format kind");
    if (header.reservedByte != PointsWriter::METADATA_RESERVED_BYTE) invalid("bad metadata reserved byte");
    if (header.reservedWord != PointsWriter::METADATA_RESERVED_WORD) invalid("bad metadata reserved word");
    metadataFlags = header.flags;
    if (metadataFlags != PointsWriter::FLAG_LEAF_MAX) invalid("unsupported metadata flags");
    leavesCount = header.leafCount;
    leafSizeMax = header.maxPointsPerLeaf;
    pointsCount = header.pointCount;
    if (leavesCount == 0) invalid("leafCount must be positive");
    if (leafSizeMax == 0) invalid("maxPointsPerLeaf must be positive");
    if (pointsCount == 0) invalid("pointCount must be positive");
    if (leavesCount != PointsWriter::leafCount(pointsCount, leafSizeMax)) invalid("leafCount does not match pointCount");

    auto directoryOff = PointsWriter::directoryOffset(directoryOffset);
    if (PointsWriter::directoryBytes(directoryOffset, leavesCount) != directory.size()) {
      invalid("truncated leaf directory");
    }
    byteSize = leaves.size() + directory.size();
    leafMins = meta + directoryOff;
    leafMaxes = leafMins + (uint64_t)leavesCount * sizeof(int64_t);
    leafPointers = leafMaxes + (uint64_t)leavesCount * sizeof(int64_t);
    leafEncodings = leafPointers + ((uint64_t)leavesCount + 1) * sizeof(uint64_t);

    if (leafFP(0) != 0) invalid("first leaf pointer must be zero");
    if (leafFP(leavesCount) != metaOffset) invalid("final leaf pointer does not match the leaves length");
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

  struct ValueRange {
    int64_t lo;
    int64_t hi;
  };

  struct OrdinalRange {
    uint64_t begin;
    uint64_t end;
  };

  struct EmissionWork {
    uint64_t leaves = 0;
    uint64_t decodedBits = 0;
    uint64_t scattered = 0;
    uint64_t bitsetWords = 0;
    uint64_t runWords = 0;
    uint64_t selects = 0;

    EmissionWork& operator+=(const EmissionWork& other) {
      leaves += other.leaves;
      decodedBits += other.decodedBits;
      scattered += other.scattered;
      bitsetWords += other.bitsetWords;
      runWords += other.runWords;
      selects += other.selects;
      return *this;
    }
  };

  uint32_t leafEncoding(uint32_t leaf) const {
    if (leaf >= leavesCount) throw std::out_of_range("PointsReader: leaf encoding index");
    return load<uint32_t>(leafEncodings + (uint64_t)leaf * sizeof(uint32_t));
  }

  // Match emitOrdinalRange's leaf invocations, without touching leaf payloads.
  EmissionWork emissionWork(uint64_t begin, uint64_t end) const {
    if (begin > end || end > pointsCount) {
      throw std::out_of_range("PointsReader: emission ordinal range");
    }
    EmissionWork work;
    if (begin == end) return work;
    uint32_t firstLeaf = (uint32_t)(begin / leafSizeMax);
    uint32_t lastLeaf = (uint32_t)((end - 1) / leafSizeMax);
    auto boundary = [&](uint32_t leaf, uint64_t first, uint64_t last) {
      uint64_t count = leafOrdinalEnd(leaf) - leafOrdinalStart(leaf);
      uint32_t encoding = leafEncoding(leaf);
      uint32_t detail = encoding >> 2;
      work.leaves++;
      switch (encoding & 3) {
        case PointsWriter::DOC_CONTIG:
          work.runWords += (detail + last - 1) / 64 - (detail + first) / 64 + 1;
          break;
        case PointsWriter::DOC_FOR:
          work.decodedBits += count * detail; // Partial ranges decode the whole leaf.
          work.scattered += last - first;
          break;
        case PointsWriter::DOC_BITSET:
          work.bitsetWords += detail;
          if (first != 0 || last != count) {
            // Rank selections plus the emission pass: conservative bounds,
            // since exact boundary word positions require reading the bitmap.
            work.bitsetWords += first == 0 ? 1 : detail;
            work.selects++;
            if (last != count) {
              work.bitsetWords += detail;
              work.selects++;
            }
          }
          break;
        default: invalid("reserved summary docid codec");
      }
    };
    uint64_t firstStart = (uint64_t)firstLeaf * leafSizeMax;
    if (firstLeaf == lastLeaf) {
      boundary(firstLeaf, begin - firstStart, end - firstStart);
      return work;
    }
    boundary(firstLeaf, begin - firstStart, leafSizeMax);
    boundary(lastLeaf, 0, end - (uint64_t)lastLeaf * leafSizeMax);

    // Interior leaves are full. Stream the packed summaries without ordinal
    // arithmetic or checked directory lookups; multiply FOR totals once.
    uint64_t forLeaves = 0, docBits = 0, bitsetWords = 0, runWords = 0;
    const char* stop = leafEncodings + (uint64_t)lastLeaf * sizeof(uint32_t);
    for (const char* p = leafEncodings + ((uint64_t)firstLeaf + 1) * sizeof(uint32_t);
         p < stop; p += sizeof(uint32_t)) {
      uint32_t encoding = load<uint32_t>(p);
      uint32_t detail = encoding >> 2;
      switch (encoding & 3) {
        case PointsWriter::DOC_CONTIG:
          runWords += (detail + leafSizeMax + 63) >> 6;
          break;
        case PointsWriter::DOC_FOR:
          forLeaves++;
          docBits += detail;
          break;
        case PointsWriter::DOC_BITSET:
          bitsetWords += detail;
          break;
        default: invalid("reserved summary docid codec");
      }
    }
    work.leaves += lastLeaf - firstLeaf - 1;
    work.decodedBits += docBits * leafSizeMax;
    work.scattered += forLeaves * leafSizeMax;
    work.bitsetWords += bitsetWords;
    work.runWords += runWords;
    return work;
  }

  struct Point {
    int64_t value;
    int32_t docid;

    bool operator==(const Point&) const = default;
  };

  // Leaf layout. count and the docid fields derive from the directory; the
  // rest is read from the leaf header.
  struct LeafInfo {
    uint16_t count;
    uint8_t docCodec;
    uint8_t docBits;
    uint8_t valueFormat;  // Bit width of the residual deltas.
    uint32_t docBase;
    uint32_t docBytes;
    uint64_t valueGcd;
    uint32_t headerBytes;

    uint64_t valueBytes() const { return SplitForCodec::byteSize(count, valueFormat); }
  };

  PointsReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : context(segmentReadContext(reader.core(), fieldInfo, UINT32_MAX)) {
    const auto* part = fieldInfo.points();
    if (!part) invalid("points index is absent");
    context.file = part->location.filenum();
    auto& core = reader.core();
    auto directory = fieldInfo.pointsDirectory();
    init(core.slice(part->leaves()), core.slice(directory, ReadClass::METADATA), directory.location.offset());
    if (leafCount() != fieldInfo.pointLeafCount() || maxPointsPerLeaf() != part->maxPointsPerLeaf
        || pointCount() != fieldInfo.pointCount()) invalid("directory counts disagree with field record");
  }

  PointsReader(const InputStream& input, uint64_t leavesOffset, uint64_t leavesBytes, uint64_t directoryBytes) {
    auto size = (uint64_t)input.size();
    if (leavesOffset > size || leavesBytes > size - leavesOffset
        || directoryBytes > size - leavesOffset - leavesBytes) invalid("points slice is out of bounds");
    auto directoryOffset = leavesOffset + leavesBytes;
    init({input.ptr(leavesOffset), leavesBytes}, {input.ptr(directoryOffset), directoryBytes}, directoryOffset);
  }

  uint64_t pointCount() const { return pointsCount; }
  uint64_t sizeInBytes() const { return byteSize; }
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

  // Unordered document occurrences in an inclusive value range. Shared by
  // key enumeration and bucket predicates; multi-valued callers deduplicate.
  template <typename Visit>
  void forEachRangeDoc(int64_t lo, int64_t hi, Visit&& visit) {
    auto fence = fenceRange(lo, hi);
    if (fence.empty) return;
    for (uint32_t leaf = fence.firstLeaf; leaf <= fence.lastLeaf; leaf++) {
      for (const auto& point : decodeLeaf(leaf)) {
        if (point.value >= lo && point.value <= hi) visit(point.docid);
      }
    }
  }

  // Benchmark-only min-directory arm. It may include one extra left leaf.
  FenceRange fenceRangeMinOnlyForBench(int64_t lo, int64_t hi) const {
    FenceRange range;
    if (hi < lo) return range;

    uint32_t leftLo = 0;
    uint32_t leftHi = leavesCount;
    while (leftLo < leftHi) {
      uint32_t mid = leftLo + (leftHi - leftLo) / 2;
      if (mid + 1 < leavesCount && leafMin(mid + 1) < lo) {
        leftLo = mid + 1;
      } else {
        leftHi = mid;
      }
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
                              std::span<int64_t> valueScratch) const {
    LeafValueBounds bounds;
    int64_t min = leafMin(leafIndex);
    int64_t max = leafMax(leafIndex);
    if (hi < min || max < lo) return bounds;
    if (lo <= min && max <= hi) return {0, leafPointCount(leafIndex)};

    uint16_t count = decodeLeafValuesInto(leafIndex, valueScratch, residualScratch);
    auto values = valueScratch.first(count);
    bounds.lower = (uint16_t)(std::lower_bound(values.begin(), values.end(), lo)
                               - values.begin());
    bounds.upper = (uint16_t)(std::upper_bound(values.begin(), values.end(), hi)
                               - values.begin());
    return bounds;
  }

  // Resolve sorted, disjoint value intervals to sorted, disjoint point
  // ordinal runs. The one-interval path is the original range lookup. For a
  // set, leaves are visited once in value order and the interval cursor
  // gallops forward with them; each touched leaf's values are decoded at most
  // once. Adjacent ordinal runs are merged when the value gap is absent from
  // the dictionary.
  size_t ordinalRanges(std::span<const ValueRange> ranges,
                       std::span<OrdinalRange> output,
                       std::span<uint32_t> residualScratch,
                       std::span<int64_t> valueScratch) const {
    if (ranges.empty()) return 0;
    if (output.size() < ranges.size()) {
      throw std::invalid_argument(
          "PointsReader: ordinal range output is too small");
    }
    assert(std::is_sorted(
        ranges.begin(), ranges.end(),
        [](const ValueRange& a, const ValueRange& b) { return a.lo < b.lo; }));

    size_t outputSize = 0;
    auto append = [&](uint64_t begin, uint64_t end) {
      if (begin == end) return;
      if (outputSize != 0 && output[outputSize - 1].end == begin) {
        output[outputSize - 1].end = end;
      } else {
        output[outputSize++] = {begin, end};
      }
    };

    FenceRange fence = fenceRange(ranges.front().lo, ranges.back().hi);
    if (fence.empty) return 0;
    if (ranges.size() == 1) {
      LeafValueBounds first = valueBounds(
          fence.firstLeaf, ranges[0].lo, ranges[0].hi,
          residualScratch, valueScratch);
      uint64_t begin = leafOrdinalStart(fence.firstLeaf) + first.lower;
      uint64_t end;
      if (fence.firstLeaf == fence.lastLeaf) {
        end = leafOrdinalStart(fence.firstLeaf) + first.upper;
      } else {
        LeafValueBounds last = valueBounds(
            fence.lastLeaf, ranges[0].lo, ranges[0].hi,
            residualScratch, valueScratch);
        end = leafOrdinalStart(fence.lastLeaf) + last.upper;
      }
      append(begin, end);
      return outputSize;
    }

    int64_t rangeCursor = 0;
    for (uint32_t leaf = fence.firstLeaf; leaf <= fence.lastLeaf; leaf++) {
      int64_t leafLo = leafMin(leaf);
      int64_t leafHi = leafMax(leaf);
      rangeCursor = screaming::gallopLowerBound(
          rangeCursor, (int64_t)ranges.size(), leafLo,
          [&](int64_t index) { return ranges[(size_t)index].hi; });
      if (rangeCursor == (int64_t)ranges.size()) break;
      if (ranges[(size_t)rangeCursor].lo > leafHi) continue;

      uint16_t count = 0;
      int64_t valueCursor = 0;
      for (size_t i = (size_t)rangeCursor;
           i < ranges.size() && ranges[i].lo <= leafHi; i++) {
        const ValueRange& range = ranges[i];
        if (range.hi < leafLo) continue;
        uint16_t begin;
        uint16_t end;
        if (range.lo <= leafLo && leafHi <= range.hi) {
          begin = 0;
          end = leafPointCount(leaf);
        } else {
          if (count == 0) {
            count = decodeLeafValuesInto(
                leaf, valueScratch, residualScratch);
          }
          valueCursor = screaming::gallopLowerBound(
              valueCursor, count, range.lo,
              [&](int64_t index) { return valueScratch[(size_t)index]; });
          begin = (uint16_t)valueCursor;
          if (range.hi == std::numeric_limits<int64_t>::max()) {
            valueCursor = count;
          } else {
            valueCursor = screaming::gallopLowerBound(
                valueCursor, count, range.hi + 1,
                [&](int64_t index) { return valueScratch[(size_t)index]; });
          }
          end = (uint16_t)valueCursor;
        }
        uint64_t leafStart = leafOrdinalStart(leaf);
        append(leafStart + begin, leafStart + end);
      }
    }
    return outputSize;
  }

  // Global ordinal of the first point whose value is at least value.
  uint64_t ordinalOfFirstAtLeast(int64_t value,
                                 std::span<uint32_t> residualScratch,
                                 std::span<int64_t> rawScratch) const {
    uint32_t lo = 0;
    uint32_t hi = leavesCount;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (leafMax(mid) < value) lo = mid + 1;
      else hi = mid;
    }
    if (lo == leavesCount) return pointsCount;
    auto bounds = valueBounds(lo, value, std::numeric_limits<int64_t>::max(),
                              residualScratch, rawScratch);
    return leafOrdinalStart(lo) + bounds.lower;
  }

  template <class EmitDoc, class EmitRun, class EmitWord>
  void emitDocids(uint32_t leafIndex, uint16_t begin, uint16_t end,
                  std::span<uint32_t> docScratch, EmitDoc emitDoc,
                  EmitRun emitRun, EmitWord emitWord) const {
    LeafInfo info = leafInfo(leafIndex);
    if (begin > end || end > info.count) {
      throw std::out_of_range("PointsReader: leaf point range");
    }
    if (begin == end) return;
    if (info.docCodec == PointsWriter::DOC_CONTIG) {
      emitRun((int32_t)(info.docBase + begin), (int32_t)(info.docBase + end));
      return;
    }
    const char* payload = points + leafFP(leafIndex) + info.headerBytes;
    if (info.docCodec == PointsWriter::DOC_BITSET) {
      uint32_t wordCount = info.docBytes / sizeof(uint64_t);
      int32_t wordBase = (int32_t)(info.docBase >> 6);
      if (begin == 0 && end == info.count) {
        for (uint32_t wordIndex = 0; wordIndex < wordCount; wordIndex++) {
          uint64_t word = load<uint64_t>(
              payload + (uint64_t)wordIndex * sizeof(uint64_t));
          if (word != 0) emitWord(wordBase + (int32_t)wordIndex, word);
        }
        return;
      }

      auto selectBit = [&](uint16_t ordinal) {
        if (ordinal == info.count) return wordCount << 6;
        uint32_t remaining = ordinal;
        for (uint32_t wordIndex = 0; wordIndex < wordCount; wordIndex++) {
          uint64_t word = load<uint64_t>(
              payload + (uint64_t)wordIndex * sizeof(uint64_t));
          uint32_t count = (uint32_t)std::popcount(word);
          if (remaining < count) {
            while (remaining-- != 0) word &= word - 1;
            return (wordIndex << 6) + (uint32_t)std::countr_zero(word);
          }
          remaining -= count;
        }
        invalid("BITSET ordinal exceeds popcount");
      };

      uint32_t firstBit = selectBit(begin);
      uint32_t endBit = selectBit(end);
      uint32_t firstWord = firstBit >> 6;
      uint32_t lastWord = (endBit - 1) >> 6;
      for (uint32_t wordIndex = firstWord; wordIndex <= lastWord; wordIndex++) {
        uint64_t word = load<uint64_t>(
            payload + (uint64_t)wordIndex * sizeof(uint64_t));
        if (wordIndex == firstWord) word &= ~0ULL << (firstBit & 63);
        if (wordIndex == lastWord && (endBit & 63) != 0) {
          word &= (1ULL << (endBit & 63)) - 1ULL;
        }
        if (word != 0) emitWord(wordBase + (int32_t)wordIndex, word);
      }
      return;
    }
    if (info.docCodec != PointsWriter::DOC_FOR) invalid("reserved docid codec");
    if (docScratch.size() < info.count) {
      throw std::invalid_argument("PointsReader: docid scratch is too small");
    }
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

  template <class EmitDoc, class EmitRun, class EmitWord>
  void emitOrdinalRange(uint64_t begin, uint64_t end,
                        std::span<uint32_t> docScratch, EmitDoc emitDoc,
                        EmitRun emitRun, EmitWord emitWord) const {
    if (begin > end || end > pointsCount) {
      throw std::out_of_range("PointsReader: point ordinal range");
    }
    while (begin < end) {
      uint32_t leafIndex = (uint32_t)(begin / leafSizeMax);
      uint64_t leafStart = leafOrdinalStart(leafIndex);
      uint64_t leafEnd = leafOrdinalEnd(leafIndex);
      uint64_t partEnd = std::min(end, leafEnd);
      emitDocids(leafIndex, (uint16_t)(begin - leafStart),
                 (uint16_t)(partEnd - leafStart), docScratch, emitDoc, emitRun,
                 emitWord);
      begin = partEnd;
    }
  }

  // Values of one leaf in ordinal order. residualScratch holds the unpacked
  // deltas when they fit 32 bits; wider deltas unpack in place in values.
  uint16_t decodeLeafValuesInto(
      uint32_t leafIndex, std::span<int64_t> values,
      std::span<uint32_t> residualScratch) const {
    LeafInfo info = leafInfo(leafIndex);
    if (values.size() < info.count || residualScratch.size() < info.count) {
      throw std::invalid_argument(
          "PointsReader: leaf value scratch is too small");
    }
    decodeValues(leafIndex, info, values.first(info.count), residualScratch);
    return info.count;
  }

  uint16_t decodeLeafInto(uint32_t leafIndex, std::span<int64_t> values,
                          std::span<uint32_t> docids,
                          std::span<uint32_t> residualScratch) const {
    LeafInfo info = leafInfo(leafIndex);
    if (values.size() < info.count || docids.size() < info.count
        || residualScratch.size() < info.count) {
      throw std::invalid_argument("PointsReader: leaf decode scratch is too small");
    }

    const char* docPayload = points + leafFP(leafIndex) + info.headerBytes;
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
    } else if (info.docCodec == PointsWriter::DOC_BITSET) {
      uint32_t size = 0;
      uint32_t wordBase = info.docBase >> 6;
      uint32_t wordCount = info.docBytes / sizeof(uint64_t);
      for (uint32_t wordIndex = 0; wordIndex < wordCount; wordIndex++) {
        uint64_t word = load<uint64_t>(
            docPayload + (uint64_t)wordIndex * sizeof(uint64_t));
        while (word != 0) {
          if (size == info.count) {
            invalid("BITSET popcount exceeds count");
          }
          uint32_t bit = (uint32_t)std::countr_zero(word);
          docids[size++] = (wordBase + wordIndex) * 64 + bit;
          word &= word - 1;
        }
      }
      if (size != info.count) invalid("BITSET popcount does not match count");
    } else {
      invalid("reserved docid codec");
    }

    decodeValues(leafIndex, info, values.first(info.count), residualScratch);
    return info.count;
  }

  void validate() const {
    uint64_t countedPoints = 0;
    std::vector<int64_t> values(leafSizeMax);
    std::vector<uint32_t> residuals(leafSizeMax);
    for (uint32_t i = 0; i < leavesCount; i++) {
      uint64_t start = leafFP(i);
      uint64_t end = leafFP(i + 1);
      if (start >= end) invalid("leaf pointers must be strictly increasing");
      if (end > metaOffset) invalid("leaf overlaps metadata");
      if (end - start < PointsWriter::LEAF_HEADER_MIN_SIZE) invalid("truncated leaf header");
      if (i > 0 && leafMin(i) < leafMin(i - 1)) invalid("leafMin is not non-decreasing");
      if (leafMax(i) < leafMin(i)) invalid("leafMax is below leafMin");
      if (i > 0 && leafMin(i) < leafMax(i - 1)) invalid("leaf value ranges are out of order");

      uint32_t encoding = leafEncoding(i);
      uint32_t detail = encoding >> 2;
      if ((encoding & 3) == PointsWriter::DOC_FOR && detail > 31) invalid("FOR docBits exceeds 31");
      if ((encoding & 3) == PointsWriter::DOC_BITSET
          && (uint64_t)detail * sizeof(uint64_t) > end - start) {
        invalid("docid payload exceeds leaf");
      }
      if (end - start < PointsWriter::leafHeaderSize((uint8_t)points[start + 4])) invalid("truncated leaf header");
      auto info = leafInfo(i);
      if ((points[start + 4] & PointsWriter::VALUE_GCD) && info.valueGcd <= 1) {
        invalid("stored valueGcd must exceed one");
      }
      if (info.docBase > (uint32_t)std::numeric_limits<int32_t>::max()) invalid("docBase exceeds int32");
      if (info.valueFormat > 64) invalid("invalid value format");
      if ((uint64_t)info.headerBytes + info.docBytes > end - start) {
        invalid("docid payload exceeds leaf");
      }
      if (info.docCodec == PointsWriter::DOC_CONTIG) {
        if (detail != (info.docBase & 63)) invalid("leaf encoding summary does not match header");
        if ((uint64_t)info.docBase + info.count - 1
            > (uint64_t)std::numeric_limits<int32_t>::max()) {
          invalid("CONTIG docids exceed int32");
        }
      } else if (info.docCodec == PointsWriter::DOC_BITSET) {
        if (info.docBytes == 0) invalid("invalid BITSET byte length");
        const char* payload = points + start + info.headerBytes;
        uint32_t wordCount = info.docBytes / sizeof(uint64_t);
        uint64_t firstWord = load<uint64_t>(payload);
        uint32_t baseBit = info.docBase & 63;
        uint64_t belowBaseMask = baseBit == 0
            ? 0 : (1ULL << baseBit) - 1ULL;
        if ((firstWord & belowBaseMask) != 0
            || (firstWord & (1ULL << baseBit)) == 0) {
          invalid("BITSET has bits below docBase or omits docBase");
        }
        uint64_t popcount = 0;
        for (uint32_t wordIndex = 0; wordIndex < wordCount; wordIndex++) {
          popcount += (uint64_t)std::popcount(load<uint64_t>(
              payload + (uint64_t)wordIndex * sizeof(uint64_t)));
        }
        if (popcount != info.count) {
          invalid("BITSET popcount does not match count");
        }
        uint64_t lastWord = load<uint64_t>(
            payload + (uint64_t)(wordCount - 1) * sizeof(uint64_t));
        if (lastWord == 0) invalid("BITSET has trailing zero words");
        uint32_t maxBit = 63 - (uint32_t)std::countl_zero(lastWord);
        uint64_t maxDocid = ((uint64_t)(info.docBase >> 6) + wordCount - 1) * 64
                          + maxBit;
        if (maxDocid > (uint64_t)std::numeric_limits<int32_t>::max()) {
          invalid("BITSET docid exceeds int32");
        }
      }
      uint64_t valueBytes = end - start - info.headerBytes - info.docBytes;
      uint64_t padding = i + 1 == leavesCount ? MAX_ALIGN - 1 : 0;
      if (valueBytes < info.valueBytes() || valueBytes - info.valueBytes() > padding) {
        invalid("invalid value payload length");
      }
      auto leafValues = std::span(values).first(info.count);
      decodeValues(i, info, leafValues, residuals);
      if (leafValues.front() != leafMin(i) || leafValues.back() != leafMax(i)) {
        invalid("leaf values do not match directory fences");
      }
      if (!std::is_sorted(leafValues.begin(), leafValues.end())) invalid("leaf values are not sorted");
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

  uint16_t leafPointCount(uint32_t leafIndex) const {
    return (uint16_t)(leafOrdinalEnd(leafIndex) - leafOrdinalStart(leafIndex));
  }

  LeafInfo leafInfo(uint32_t leafIndex) const {
    uint32_t encoding = leafEncoding(leafIndex);
    uint32_t detail = encoding >> 2;
    LeafInfo info{};
    info.count = leafPointCount(leafIndex);
    info.docCodec = (uint8_t)(encoding & 3);
    if (info.docCodec == PointsWriter::DOC_FOR) {
      info.docBits = (uint8_t)detail;
      info.docBytes = (uint32_t)ForCodec::byteSize(info.count, info.docBits);
    } else if (info.docCodec == PointsWriter::DOC_BITSET) {
      info.docBytes = detail * (uint32_t)sizeof(uint64_t);
    } else if (info.docCodec != PointsWriter::DOC_CONTIG) {
      invalid("reserved docid codec");
    }
    const char* header = points + leafFP(leafIndex);
    info.docBase = load<uint32_t>(header);
    auto valueByte = (uint8_t)header[4];
    info.valueFormat = valueByte & PointsWriter::VALUE_BITS_MASK;
    info.valueGcd = valueByte & PointsWriter::VALUE_GCD ? load<uint64_t>(header + 5) : 1;
    info.headerBytes = PointsWriter::leafHeaderSize(valueByte);
    return info;
  }

  std::vector<Point> decodeLeaf(uint32_t leafIndex) const {
    std::vector<int64_t> values(leafSizeMax);
    std::vector<uint32_t> docids(leafSizeMax);
    std::vector<uint32_t> residuals(leafSizeMax);
    uint16_t count = decodeLeafInto(leafIndex, values, docids, residuals);
    std::vector<Point> decoded(count);
    for (uint16_t i = 0; i < count; i++) decoded[i] = {values[i], (int32_t)docids[i]};
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

private:
  // Unpack the residual deltas, prefix-sum them and restore values from the
  // directory leafMin and the leaf gcd.
  void decodeValues(uint32_t leafIndex, const LeafInfo& info, std::span<int64_t> values,
                    std::span<uint32_t> residualScratch) const {
    uint64_t min = (uint64_t)leafMin(leafIndex);
    uint64_t gcd = info.valueGcd;
    uint32_t count = info.count;
    if (info.valueFormat == 0) {
      std::fill(values.begin(), values.end(), (int64_t)min);
      return;
    }
    if (info.valueFormat > 64) invalid("invalid value format");
    const char* payload = points + leafFP(leafIndex) + info.headerBytes + info.docBytes;
    uint64_t residual = 0;
    if (info.valueFormat <= 32) {
      ForCodec::decode(payload, count, info.valueFormat, residualScratch);
      for (uint32_t i = 0; i < count; i++) {
        residual += residualScratch[i];
        values[i] = (int64_t)(min + residual * gcd);
      }
      return;
    }
    // int64_t and uint64_t may alias, so wide deltas unpack in place.
    std::span<uint64_t> deltas{reinterpret_cast<uint64_t*>(values.data()), count};
    SplitForCodec::decode(payload, count, info.valueFormat, deltas);
    for (uint32_t i = 0; i < count; i++) {
      residual += deltas[i];
      deltas[i] = min + residual * gcd;
    }
  }
};

} // namespace luxir
