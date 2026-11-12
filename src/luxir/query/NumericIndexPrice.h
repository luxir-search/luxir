// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "luxir/query/PointsMaterialize.h"
#include "luxir/reader/PointsReader.h"

namespace luxir {

// Numeric indexed producers, in point emissions. Geo retains the count-based
// PointsMaterialize price. Fits use the pinned 1M-document forced-arm board
// (2026-11-12), at about 0.65 ns/point; verification remains 20 points.
struct NumericIndexPrice {
  // Boolean d10/d50 l1 residuals put destination work at 2.7-3.6 points/word.
  // Their alone delta gives about 12 points/additional leaf. Shuffled date
  // r10/r50 give about 1.1 points/FOR value at 20 bits: decode plus scatter.
  static constexpr double DESTINATION_WORD_POINTS = 3;
  static constexpr double LEAF_POINTS = 12;
  static constexpr double DECODE_BIT_POINTS = 0.01;
  static constexpr double SCATTER_POINTS = 0.9;
  static constexpr double SOURCE_WORD_POINTS = 1;
  static constexpr double RUN_WORD_POINTS = 1;
  static constexpr double SELECT_POINTS = 64;

  // Directory arithmetic only: no leaf entries or payloads are touched.
  // Count invocations, including repeated boundary leaves of disjoint ranges.
  static double pointsLowerBound(std::span<const PointsReader::OrdinalRange> ranges,
                                 uint64_t pointCount, uint32_t leafSize, uint64_t emitted,
                                 int32_t maxDoc, bool complement = false, uint64_t baseDocs = 0) {
    if (!complement && !PointsMaterialize::useBitset(emitted, maxDoc)) {
      return PointsMaterialize::price(emitted, maxDoc);
    }
    uint64_t leaves = 0, cursor = 0;
    auto add = [&](uint64_t begin, uint64_t end) {
      if (begin < end) leaves += (end - 1) / leafSize - begin / leafSize + 1;
    };
    for (const auto& range : ranges) {
      if (complement) add(cursor, range.begin);
      else add(range.begin, range.end);
      cursor = range.end;
    }
    if (complement) add(cursor, pointCount);
    return points(PointsReader::EmissionWork{leaves}, emitted, maxDoc, complement, baseDocs);
  }

  static double points(const PointsReader::EmissionWork& work, uint64_t emitted,
                       int32_t maxDoc, bool complement = false, uint64_t baseDocs = 0) {
    // The calibrated array price includes traversal, expansion and sorting.
    if (!complement && !PointsMaterialize::useBitset(emitted, maxDoc)) {
      return PointsMaterialize::price(emitted, maxDoc);
    }
    return (DESTINATION_WORD_POINTS + (complement ? PointsMaterialize::WORD_POINTS : 0))
            * FixedBitSet::sizeInWords(maxDoc)
        + LEAF_POINTS * work.leaves + DECODE_BIT_POINTS * work.decodedBits
        + SCATTER_POINTS * (work.scattered + baseDocs)
        + SOURCE_WORD_POINTS * work.bitsetWords + RUN_WORD_POINTS * work.runWords
        + SELECT_POINTS * work.selects;
  }
};

} // namespace luxir
