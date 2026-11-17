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
#include "luxir/util/geo.h"

namespace luxir {

// One packed int64 column value per geographic point. Point ranks coincide
// with value ranks; a single-valued column also uses document ranks.
struct GeoPointSource {
  using PointTraits = BKDInt32Traits;
  using Reader = BasicBKDReader<PointTraits>;

  static constexpr double BOX_MATCH_COST = 2.0;
  // Testing one point of a crossing leaf against a box, in point emissions.
  // Measured by forcing the BKD for lone boxes under a 35-candidate lead
  // (5M docs, one segment, 2026-10-28): 0.69 ns per emitted point, 1.3 ns
  // per crossing point (nine boxes, emitted 0.8k-2.5M, crossing 1.5k-88k).
  // Relations add their exact test's extra (BKDPointCost).
  static constexpr double CROSSING_POINTS = 2.0;

  static constexpr int64_t& sparseVerifyArms = SkipStats::geoSparseVerifyArms;
  static constexpr int64_t& bkdArms = SkipStats::geoBKDArms;
  static constexpr int64_t& scanArms = SkipStats::geoScanArms;

  static IntColReader* openColumn(MemPool& pool, PostingsReader& postingsReader,
                                  const SegFieldInfo& info, std::string_view queryName) {
    if (info.type != FieldType::GEO_POINT) {
      throw std::runtime_error(std::string(queryName) + " requires a GEO_POINT field");
    }
    return pool.make<IntColReader>(postingsReader, info);
  }

  static uint64_t pointCount(const SegFieldInfo& info) {
    return (uint64_t)info.numValues;
  }

  static uint64_t pointCount(const IntColReader& column) {
    return (uint64_t)column.numValues();
  }

  // The column layout guarantees exactly one point per populated document.
  static bool onePointPerDoc(const SegFieldInfo& info) {
    return info.monoLoc.offset() == 0;
  }

  static bool onePointPerDoc(const IntColReader& column) {
    return !column.multiValued();
  }

  // Used when the layout does not guarantee one point per document.
  static std::pair<int64_t, int64_t> pointRanks(IntColReader& column, int32_t docRank) {
    return column.getStartEndValueRank(docRank);
  }

  static std::pair<PointTraits::Scalar, PointTraits::Scalar> pointAt(
      auto& values, int64_t pointRank) {
    int64_t packed = values.valueAt(pointRank);
    return {geo::unpackLatitude(packed), geo::unpackLongitude(packed)};
  }

  static void validatePointCount(const Reader& bkd, const IntColReader& column,
                                 std::string_view queryName) {
    if (bkd.pointCount() != pointCount(column)) {
      throw std::runtime_error(std::string(queryName) + ": points/column value count mismatch");
    }
  }

  // A box test is two value compares; other relations scale it by their
  // average test. Multiple points increase the scorer's verification work.
  static float matchCost(const IntColReader& column, BKDPointCost cost) {
    float point = (float)(BOX_MATCH_COST * (1.0 + cost.average / BOX_TEST_POINTS));
    if (onePointPerDoc(column)) return point;
    int64_t docs = column.docsWithValue();
    return docs > 0 ? point * (float)pointCount(column) / (float)docs : point;
  }

  // Verifying a candidate costs a column check plus the relation's average
  // test extra. The arm comparison uses this flat price for multivalued fields.
  static double verifyPoints(BKDPointCost cost) {
    return PointsMaterialize::VERIFY_POINTS + cost.average;
  }
};

} // namespace luxir
