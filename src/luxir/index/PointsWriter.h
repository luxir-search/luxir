#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

#include <boost/sort/spreadsort/integer_sort.hpp>

#include "luxir/codec/Codec.h"
#include "luxir/store/OutputStream.h"

namespace luxir {

// Canonical (value, docid) point sort shared by the flush and merge builders.
// Two-level: radix by value, then re-sort each equal-value run by docid.
// integer_sort's comparator MUST order exactly by the shifted key - it never
// runs the comparator inside equal-key buckets, so a docid tie-break in the
// comparator is silently ignored (measured output mismatch on low-cardinality
// data, 2026-07-10). Never slower than std::sort in the flush-shape
// microbench; 7.7x on docid-correlated values.
template <class Point>
void sortPointsByValueDocid(std::span<Point> points) {
  boost::sort::spreadsort::integer_sort(points.begin(), points.end(),
      [](const Point& p, unsigned offset) { return p.value >> offset; },
      [](const Point& a, const Point& b) { return a.value < b.value; });
  auto runBegin = points.begin();
  while (runBegin != points.end()) {
    auto runEnd = runBegin + 1;
    while (runEnd != points.end() && runEnd->value == runBegin->value) ++runEnd;
    if (runEnd - runBegin > 1) {
      boost::sort::spreadsort::integer_sort(runBegin, runEnd,
          [](const Point& p, unsigned offset) { return p.docid >> offset; },
          [](const Point& a, const Point& b) { return a.docid < b.docid; });
    }
    runBegin = runEnd;
  }
}

class PointsWriter {
public:
  static constexpr uint32_t MAGIC = 0x31545053;
  static constexpr uint16_t VERSION = 1;
  static constexpr uint8_t FORMAT_KIND = 1;
  static constexpr uint32_t FLAG_LEAF_MAX = 1;
  static constexpr uint16_t DEFAULT_MAX_POINTS_PER_LEAF = 512;
  static constexpr uint32_t FIXED_HEADER_SIZE = 28;
  static constexpr uint32_t LEAF_HEADER_SIZE = 32;
  static constexpr uint8_t DOC_CONTIG = 0;
  static constexpr uint8_t DOC_FOR = 1;
  // Codec value 2 remains reserved. BITSET is valid only when docids are
  // strictly ascending in ordinal order, which also excludes duplicates.
  // emitDocids serves partial ordinal ranges, and ascending order makes
  // ordinal i the i-th set bit so boundary ranges remain word-emittable.
  // Payload word w is absolute word (docBase >> 6) + w. docBytes covers
  // through the word containing maxDocid, with edge bits outside the span 0.
  static constexpr uint8_t DOC_BITSET = 3;

  struct Options {
    uint16_t maxPointsPerLeaf = DEFAULT_MAX_POINTS_PER_LEAF;
    bool bitsetDocids = true;
    bool valueGcd = true;
  };

private:
  struct BufferedPoint {
    int64_t value;
    int32_t docid;
  };

  OutputStream& out;
  size_t pointsStart;
  Options options;
  uint64_t pointCount = 0;
  bool finished = false;
  bool hasLastPoint = false;
  int64_t lastValue = 0;
  int32_t lastDocid = 0;
  std::vector<BufferedPoint> leaf;
  std::vector<int64_t> leafMin;
  std::vector<int64_t> leafMax;
  std::vector<uint64_t> leafFP;
  std::vector<uint32_t> residuals;
  std::vector<char> encodedDocs;
  std::vector<char> encodedValues;
  std::vector<uint64_t> docWords;

  static void writeU16(OutputStream& target, uint16_t value) {
    target.write(&value, sizeof(value));
  }

  static void writeU32(OutputStream& target, uint32_t value) {
    target.write(&value, sizeof(value));
  }

  static void writeU64(OutputStream& target, uint64_t value) {
    target.write(&value, sizeof(value));
  }

  static void encode(std::span<uint32_t> values, uint8_t bits, std::vector<char>& target) {
    uint64_t byteSize = LuxirSIMDFor::byteSize((uint32_t)values.size(), bits);
    if (byteSize > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("PointsWriter: encoded leaf exceeds uint32");
    }
    target.resize((size_t)byteSize);
    uint32_t encodedSize = (uint32_t)target.size();
    IndexCodec::numericCodec.encodeWithMeta(values.data(), (uint32_t)values.size(),
                                             target.data(), encodedSize, 0, bits);
    assert(encodedSize == byteSize);
    target.resize(encodedSize);
  }

  void flushLeaf() {
    if (leaf.empty()) return;
    if (leafFP.size() >= (size_t)std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("PointsWriter: too many leaves");
    }

    uint64_t leafOffset = (uint64_t)(out.size() - pointsStart);
    leafFP.push_back(leafOffset);
    leafMin.push_back(leaf.front().value);
    leafMax.push_back(leaf.back().value);

    int64_t valueMin = leaf.front().value;
    int64_t valueMax = leaf.back().value;
    uint64_t valueGcd = options.valueGcd ? 0 : 1;
    if (options.valueGcd) {
      for (const auto& point : leaf) {
        if (valueGcd == 1) break;
        valueGcd = std::gcd(
            valueGcd, (uint64_t)point.value - (uint64_t)valueMin);
      }
    }
    if (valueGcd == 0) valueGcd = 1;
    uint64_t valueRange = ((uint64_t)valueMax - (uint64_t)valueMin) / valueGcd;
    uint8_t valueFormat = (uint8_t)std::bit_width(valueRange);

    uint32_t docBase = (uint32_t)leaf.front().docid;
    for (const auto& point : leaf) docBase = std::min(docBase, (uint32_t)point.docid);

    bool contiguous = true;
    for (size_t i = 0; i < leaf.size(); i++) {
      if ((uint64_t)leaf[i].docid != (uint64_t)docBase + i) {
        contiguous = false;
        break;
      }
    }

    bool strictlyAscending = true;
    for (size_t i = 1; i < leaf.size(); i++) {
      if (leaf[i - 1].docid >= leaf[i].docid) {
        strictlyAscending = false;
        break;
      }
    }

    uint8_t docCodec = contiguous ? DOC_CONTIG : DOC_FOR;
    uint8_t docBits = 0;
    encodedDocs.clear();
    if (!contiguous) {
      residuals.resize(leaf.size());
      uint32_t maxResidual = 0;
      for (size_t i = 0; i < leaf.size(); i++) {
        uint32_t residual = (uint32_t)leaf[i].docid - docBase;
        residuals[i] = residual;
        maxResidual = std::max(maxResidual, residual);
      }
      docBits = (uint8_t)std::bit_width(maxResidual);
      assert(docBits <= 31);
      encode(residuals, docBits, encodedDocs);

      uint32_t maxDocid = (uint32_t)leaf.front().docid;
      for (const auto& point : leaf) {
        maxDocid = std::max(maxDocid, (uint32_t)point.docid);
      }
      uint64_t firstWord = (uint64_t)docBase >> 6;
      uint64_t lastWord = (uint64_t)maxDocid >> 6;
      uint64_t bitsetBytes = (lastWord - firstWord + 1) * sizeof(uint64_t);
      if (options.bitsetDocids && strictlyAscending
          && bitsetBytes < encodedDocs.size()) {
        docCodec = DOC_BITSET;
        docBits = 0;
        docWords.assign((size_t)(lastWord - firstWord + 1), 0);
        for (const auto& point : leaf) {
          uint32_t docid = (uint32_t)point.docid;
          size_t word = (size_t)(((uint64_t)docid >> 6) - firstWord);
          docWords[word] |= 1ULL << (docid & 63);
        }
        encodedDocs.resize((size_t)bitsetBytes);
        memcpy(encodedDocs.data(), docWords.data(), encodedDocs.size());
      }
    }

    encodedValues.clear();
    if (valueFormat <= 32) {
      residuals.resize(leaf.size());
      for (size_t i = 0; i < leaf.size(); i++) {
        uint32_t residual = (uint32_t)(((uint64_t)leaf[i].value - (uint64_t)valueMin) / valueGcd);
        residuals[i] = residual;
        assert((int64_t)((uint64_t)residual * valueGcd + (uint64_t)valueMin) == leaf[i].value);
      }
      encode(residuals, valueFormat, encodedValues);
    }

    size_t headerStart = out.size();
    writeU16(out, (uint16_t)leaf.size());
    out.write((char)docCodec);
    out.write((char)docBits);
    out.write((char)valueFormat);
    out.write((char)0x11);
    out.write((char)0x11);
    out.write((char)0x11);
    writeU32(out, docBase);
    writeU32(out, (uint32_t)encodedDocs.size());
    writeU64(out, (uint64_t)valueMin);
    writeU64(out, valueGcd);
    assert(out.size() - headerStart == LEAF_HEADER_SIZE);

    out.write(encodedDocs.data(), encodedDocs.size());
    if (valueFormat <= 32) {
      out.write(encodedValues.data(), encodedValues.size());
    } else {
      for (const auto& point : leaf) writeU64(out, (uint64_t)point.value);
    }
    assert((uint64_t)(out.size() - pointsStart) > leafOffset);
    leaf.clear();
  }

public:
  struct Data {
    seg_location pointsLoc;
    int64_t pointsMetaOff;
    uint64_t pointCount;
    uint32_t leafCount;
  };

  explicit PointsWriter(OutputStream& out) : PointsWriter(out, Options{}) {}

  PointsWriter(OutputStream& out, Options options)
      : out(out), pointsStart(out.size()), options(options) {
    static_assert(std::endian::native == std::endian::little);
    if (options.maxPointsPerLeaf == 0) {
      throw std::invalid_argument("PointsWriter: maxPointsPerLeaf must be positive");
    }
    leaf.reserve(options.maxPointsPerLeaf);
  }

  void addPoint(int64_t value, int32_t docid) {
    if (finished) throw std::logic_error("PointsWriter: addPoint after finish");
    if (docid < 0) throw std::invalid_argument("PointsWriter: docid must be non-negative");
    if (hasLastPoint && (value < lastValue || (value == lastValue && docid < lastDocid))) {
      throw std::invalid_argument("PointsWriter: points are not sorted by (value, docid)");
    }
    if (pointCount == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("PointsWriter: too many points");
    }
    leaf.push_back({value, docid});
    pointCount++;
    hasLastPoint = true;
    lastValue = value;
    lastDocid = docid;
    if (leaf.size() == options.maxPointsPerLeaf) flushLeaf();
  }

  Data finish() {
    if (finished) throw std::logic_error("PointsWriter: finish called twice");
    if (pointCount == 0) throw std::invalid_argument("PointsWriter: empty input");
    flushLeaf();
    finished = true;

    uint64_t pointsMetaOff = (uint64_t)(out.size() - pointsStart);
    leafFP.push_back(pointsMetaOff);
    assert(!leafMin.empty());
    assert(leafMin.size() == leafMax.size());
    assert(leafFP.size() == leafMin.size() + 1);
    assert(leafFP.back() == pointsMetaOff);
    assert(leafMin.size() <= std::numeric_limits<uint32_t>::max());

    writeU32(out, MAGIC);
    writeU16(out, VERSION);
    out.write((char)FORMAT_KIND);
    out.write((char)0x11);
    writeU32(out, FLAG_LEAF_MAX);
    writeU32(out, (uint32_t)leafMin.size());
    writeU16(out, options.maxPointsPerLeaf);
    writeU16(out, 0x1111);
    writeU64(out, pointCount);
    assert((uint64_t)(out.size() - pointsStart) == pointsMetaOff + FIXED_HEADER_SIZE);
    out.align(8);
    out.write(leafMin.data(), leafMin.size() * sizeof(int64_t));
    out.write(leafMax.data(), leafMax.size() * sizeof(int64_t));
    out.write(leafFP.data(), leafFP.size() * sizeof(uint64_t));

    return {seg_location(out.streamNumber, pointsStart), (int64_t)pointsMetaOff,
            pointCount, (uint32_t)leafMin.size()};
  }
};

} // namespace luxir
