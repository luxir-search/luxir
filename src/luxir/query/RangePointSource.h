// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "luxir/query/PointsMaterialize.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/schema/FieldType.h"

namespace luxir {

// Consecutive (min, max) column values form one point. Point ranks count
// pairs independently of the physical column's value ranks.
struct RangePointSource {
  using PointTraits = BKDInt64Traits;
  using Reader = BasicBKDReader<PointTraits>;

  // Geo box constants, unmeasured for this source's 16-byte points.
  static constexpr double BOX_MATCH_COST = 2.0;
  static constexpr double BOX_TEST_POINTS = luxir::BOX_TEST_POINTS;
  static constexpr double VERIFY_POINTS = PointsMaterialize::VERIFY_POINTS;
  static constexpr double CROSSING_POINTS = 2.0;

  static constexpr int64_t& sparseVerifyArms = SkipStats::rangeFieldSparseVerifyArms;
  static constexpr int64_t& bkdArms = SkipStats::rangeFieldBKDArms;
  static constexpr int64_t& scanArms = SkipStats::rangeFieldScanArms;

  static IntColReader* openColumn(MemPool& pool, PostingsReader& postingsReader,
                                  const SegFieldInfo& info, std::string_view queryName) {
    if (info.type != FieldType::INTERVAL) {
      throw std::runtime_error(std::string(queryName) + " requires a range field");
    }
    auto* column = pool.make<IntColReader>(postingsReader, info);
    if (!column->multiValued() || column->numValues() % 2 != 0) {
      throw std::runtime_error(std::string(queryName) + ": malformed range column");
    }
    return column;
  }

  static uint64_t pointCount(const SegFieldInfo& info) {
    return (uint64_t)info.numValues / 2;
  }

  static uint64_t pointCount(const IntColReader& column) {
    return (uint64_t)column.numValues() / 2;
  }

  // Every populated document holds at least one pair. Equality of the total
  // value count to twice the document count therefore means one pair each.
  static bool onePointPerDoc(const SegFieldInfo& info) {
    return info.numValues == 2 * (int64_t)info.docsWithField;
  }

  static bool onePointPerDoc(const IntColReader& column) {
    return column.numValues() == 2 * (int64_t)column.docsWithValue();
  }

  // Used when documents can hold multiple points.
  static std::pair<int64_t, int64_t> pointRanks(IntColReader& column, int32_t docRank) {
    auto [start, end] = column.getStartEndValueRank(docRank);
    return {start / 2, end / 2};
  }

  static std::pair<PointTraits::Scalar, PointTraits::Scalar> pointAt(
      auto& values, int64_t pointRank) {
    return {values.valueAt(pointRank * 2), values.valueAt(pointRank * 2 + 1)};
  }

  // The point of the iterator's document when it holds exactly one pair.
  static std::pair<PointTraits::Scalar, PointTraits::Scalar> docPoint(auto& iter) {
    return pointAt(iter.values(), iter.rank());
  }
};

} // namespace luxir
