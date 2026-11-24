// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "luxir/query/BKDQueryWeight.h"
#include "luxir/query/GeoPointSource.h"
#include "luxir/query/Query.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/util/geo.h"
#include "CatalogPredicates.h"

namespace luxir {

// Inclusive latitude/longitude box over packed GEO_POINT column values.
// Bounds are supplied in degrees and quantized exactly like Lucene's
// LatLonPoint.newBoxQuery.
class GeoBoxQuery final : public Query {
  std::string_view field;
  geo::EncodedBox box;

public:
  bool recoverableAsPredicate() const noexcept override { return true; }

  static constexpr std::string_view QUERY_NAME = "GeoBoxQuery";
  using PointSource = GeoPointSource;
  using Weight = BKDQueryWeight<GeoBoxQuery, BKDBoxRelation>;

  GeoBoxQuery(std::string_view field, double minLat, double maxLat,
              double minLon, double maxLon)
    : Query(QueryKind::GEO_BOX), field(field), box(minLat, maxLat, minLon, maxLon) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const GeoBoxQuery&>(other);
    return field == rhs.field && box == rhs.box;
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, box.latMin);
    value = mixHash(value, box.latMax);
    value = mixHash(value, box.lonMin);
    value = mixHash(value, box.lonMax);
    return mixHash(value, box.empty());
  }

  std::string_view getField() const { return field; }
  int32_t getMinLatitude() const { return box.latMin; }
  int32_t getMaxLatitude() const { return box.latMax; }
  int32_t getMinLongitude() const { return box.lonMin; }
  int32_t getMaxLongitude() const { return box.lonMax; }
  bool isEmpty() const { return box.empty(); }

  MatchState mayMatch(const CatalogSummaryView& view) const override {
    if (box.empty()) {
      view.prove("empty geo box");
      return MatchState::EMPTY;
    }
    return proof::geoMatch(view, field, box.latMin, box.latMax, box.lonMin, box.lonMax);
  }
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendInt32(box.latMin);
    out.appendInt32(box.latMax);
    out.appendInt32(box.lonMin);
    out.appendInt32(box.lonMax);
    out.appendBool(box.empty());
    return FilterKeyScope::SEGMENT_STABLE;
  }

  BKDBoxRelation makeRelation() const {
    return {box.latMin, box.latMax, box.lonMin, box.lonMax};
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir
