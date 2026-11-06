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
#include "luxir/util/MemPool.h"
#include "luxir/util/geo.h"

namespace luxir {

// Matches GEO_POINT values inside any of several polygons, each with optional
// holes (see geo::Polygon for the edge and containment rules). The vertices
// are borrowed and must outlive the query; a ring may repeat its first vertex
// at the end. The relation's state is built once, in the constructor's pool.
class GeoPolygonQuery final : public Query {
  std::string_view field;
  // The rings as validated, each without a repeated closing vertex.
  std::span<const geo::PolygonRings> polygons;
  BKDPolygonRelation relation;

  static bool sameVertex(const geo::LatLon& a, const geo::LatLon& b) {
    return a.lat == b.lat && a.lon == b.lon;
  }

  // Checks every coordinate, drops a repeated closing vertex, and requires
  // three distinct vertices in what remains.
  static std::span<const geo::LatLon> checkRing(
      std::span<const geo::LatLon> ring) {
    for (const auto& point : ring) {
      geo::checkLatitude(point.lat);
      geo::checkLongitude(point.lon);
    }
    if (ring.size() > 1 && sameVertex(ring.front(), ring.back())) {
      ring = ring.first(ring.size() - 1);
    }
    auto second = std::ranges::find_if(ring, [&](const geo::LatLon& point) {
      return !sameVertex(point, ring.front());
    });
    bool third = second != ring.end()
        && std::any_of(second + 1, ring.end(), [&](const geo::LatLon& point) {
             return !sameVertex(point, ring.front()) && !sameVertex(point, *second);
           });
    if (!third) {
      throw std::invalid_argument("polygon rings need at least 3 distinct vertices");
    }
    return ring;
  }

  static std::span<const geo::PolygonRings> checkPolygons(
      MemPool& pool, std::span<const geo::PolygonRings> polygons) {
    if (polygons.empty()) {
      throw std::invalid_argument("geo_polygon needs at least one polygon");
    }
    auto checked = pool.make_span<geo::PolygonRings>(polygons.size());
    for (size_t i = 0; i < polygons.size(); i++) {
      auto holes = pool.make_span<std::span<const geo::LatLon>>(polygons[i].holes.size());
      for (size_t h = 0; h < holes.size(); h++) holes[h] = checkRing(polygons[i].holes[h]);
      checked[i] = {checkRing(polygons[i].outer), holes};
    }
    return checked;
  }

  static bool sameRing(std::span<const geo::LatLon> a,
                       std::span<const geo::LatLon> b) {
    return std::ranges::equal(a, b, [](const geo::LatLon& x, const geo::LatLon& y) {
      return std::bit_cast<uint64_t>(x.lat) == std::bit_cast<uint64_t>(y.lat)
          && std::bit_cast<uint64_t>(x.lon) == std::bit_cast<uint64_t>(y.lon);
    });
  }

  static bool samePolygon(const geo::PolygonRings& a, const geo::PolygonRings& b) {
    return sameRing(a.outer, b.outer)
        && std::ranges::equal(a.holes, b.holes, sameRing);
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

  GeoPolygonQuery(MemPool& pool, std::string_view field,
                  std::span<const geo::PolygonRings> polygons)
      : Query(QueryKind::GEO_POLYGON), field(field),
        polygons(checkPolygons(pool, polygons)),
        relation(BKDPolygonRelation::build(pool, this->polygons)) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const GeoPolygonQuery&>(other);
    return field == rhs.field
        && std::ranges::equal(polygons, rhs.polygons, samePolygon);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    return mixSampledSequence(value, polygons,
        [](uint64_t seed, const geo::PolygonRings& polygon) {
          seed = mixRing(seed, polygon.outer);
          return mixSampledSequence(seed, polygon.holes,
              [](uint64_t inner, std::span<const geo::LatLon> hole) {
                return mixRing(inner, hole);
              });
        });
  }

  std::string_view getField() const { return field; }
  std::span<const geo::PolygonRings> getPolygons() const { return polygons; }
  bool isEmpty() const { return false; }
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendSize(polygons.size());
    for (const auto& polygon : polygons) {
      appendRing(out, polygon.outer);
      out.appendSize(polygon.holes.size());
      for (auto hole : polygon.holes) appendRing(out, hole);
    }
    return FilterKeyScope::SEGMENT_STABLE;
  }

  const BKDPolygonRelation& makeRelation() const { return relation; }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir
