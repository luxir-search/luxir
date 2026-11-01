// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

#include "luxir/query/GeoQuery.h"
#include "luxir/query/Query.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/util/geo.h"

namespace luxir {

// Matches GEO_POINT values inside a polygon with optional holes (see
// geo::Polygon for the edge and containment rules). The rings are borrowed
// and must outlive the query; a ring may repeat its first vertex at the end.
class GeoPolygonQuery final : public Query {
  std::string_view field;
  geo::Polygon polygon;

  static std::span<const geo::LatLon> checkRing(
      std::span<const geo::LatLon> ring) {
    if (ring.size() > 1 && ring.front().lat == ring.back().lat
        && ring.front().lon == ring.back().lon) {
      ring = ring.first(ring.size() - 1);
    }
    if (ring.size() < 3) {
      throw std::invalid_argument("polygon rings need at least 3 distinct vertices");
    }
    for (const auto& point : ring) {
      geo::checkLatitude(point.lat);
      geo::checkLongitude(point.lon);
    }
    return ring;
  }

  static bool sameRing(std::span<const geo::LatLon> a,
                       std::span<const geo::LatLon> b) {
    return std::ranges::equal(a, b, [](const geo::LatLon& x, const geo::LatLon& y) {
      return std::bit_cast<uint64_t>(x.lat) == std::bit_cast<uint64_t>(y.lat)
          && std::bit_cast<uint64_t>(x.lon) == std::bit_cast<uint64_t>(y.lon);
    });
  }

  static uint64_t mixRing(uint64_t seed, std::span<const geo::LatLon> ring) {
    return mixSampledSequence(seed, ring, [](uint64_t value, const geo::LatLon& point) {
      value = mixHash(value, std::bit_cast<uint64_t>(point.lat));
      return mixHash(value, std::bit_cast<uint64_t>(point.lon));
    });
  }

  static void appendRing(FilterKeyBuilder& out, std::span<const geo::LatLon> ring) {
    out.appendSize(ring.size());
    for (const auto& point : ring) {
      out.appendDouble(point.lat);
      out.appendDouble(point.lon);
    }
  }

public:
  bool recoverableAsPredicate() const noexcept override { return true; }

  static constexpr std::string_view QUERY_NAME = "GeoPolygonQuery";
  using Weight = GeoQueryWeight<GeoPolygonQuery, BKDPolygonRelation>;

  // `holes` is rewritten in place to drop each ring's closing vertex.
  GeoPolygonQuery(std::string_view field, std::span<const geo::LatLon> outer,
                  std::span<std::span<const geo::LatLon>> holes)
      : Query(QueryKind::GEO_POLYGON), field(field) {
    polygon.outer = checkRing(outer);
    for (auto& hole : holes) hole = checkRing(hole);
    polygon.holes = holes;
    polygon.box = {polygon.outer[0].lat, polygon.outer[0].lat,
                   polygon.outer[0].lon, polygon.outer[0].lon};
    for (const auto& point : polygon.outer) {
      polygon.box.minLat = std::min(polygon.box.minLat, point.lat);
      polygon.box.maxLat = std::max(polygon.box.maxLat, point.lat);
      polygon.box.minLon = std::min(polygon.box.minLon, point.lon);
      polygon.box.maxLon = std::max(polygon.box.maxLon, point.lon);
    }
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const GeoPolygonQuery&>(other);
    if (field != rhs.field || !sameRing(polygon.outer, rhs.polygon.outer)
        || polygon.holes.size() != rhs.polygon.holes.size()) {
      return false;
    }
    for (size_t i = 0; i < polygon.holes.size(); i++) {
      if (!sameRing(polygon.holes[i], rhs.polygon.holes[i])) return false;
    }
    return true;
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixRing(value, polygon.outer);
    return mixSampledSequence(value, polygon.holes,
        [](uint64_t seed, std::span<const geo::LatLon> hole) {
          return mixRing(seed, hole);
        });
  }

  std::string_view getField() const { return field; }
  const geo::Polygon& getPolygon() const { return polygon; }
  bool isEmpty() const { return false; }
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    appendRing(out, polygon.outer);
    out.appendSize(polygon.holes.size());
    for (auto hole : polygon.holes) appendRing(out, hole);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  BKDPolygonRelation makeRelation() const { return BKDPolygonRelation(polygon); }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir
