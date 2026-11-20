// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "luxir/reader/BKDReader.h"
#include "luxir/util/geo.h"

namespace luxir::test {

// BKDPolygonRelation's answers by scanning every ring edge, with no edge
// trees and no grid: the reference the indexed relation must reproduce
// exactly. Rings are borrowed and must not repeat their first vertex.
class LinearPolygonRelation {
  struct Part {
    geo::PolygonRings rings;
    geo::BoundingBox box;
    int32_t latMin;
    int32_t latMax;
    int32_t lonMin;
    int32_t lonMax;
  };
  std::vector<Part> parts;

  static bool ringContains(std::span<const geo::LatLon> ring, double lat, double lon) {
    bool inside = false;
    size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
      const geo::LatLon& a = ring[i];
      const geo::LatLon& b = ring[j];
      if ((a.lat > lat) != (b.lat > lat)
          && lon < (b.lon - a.lon) * (lat - a.lat) / (b.lat - a.lat) + a.lon) {
        inside = !inside;
      }
    }
    return inside;
  }

  static double orient(double ax, double ay, double bx, double by, double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  }

  static bool segmentsTouch(double ax, double ay, double bx, double by,
                            double cx, double cy, double dx, double dy) {
    double d1 = orient(cx, cy, dx, dy, ax, ay);
    double d2 = orient(cx, cy, dx, dy, bx, by);
    double d3 = orient(ax, ay, bx, by, cx, cy);
    double d4 = orient(ax, ay, bx, by, dx, dy);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0))
        && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
      return true;
    }
    auto onSegment = [](double px, double py, double qx, double qy, double rx, double ry) {
      return std::min(px, qx) <= rx && rx <= std::max(px, qx)
          && std::min(py, qy) <= ry && ry <= std::max(py, qy);
    };
    return (d1 == 0 && onSegment(cx, cy, dx, dy, ax, ay))
        || (d2 == 0 && onSegment(cx, cy, dx, dy, bx, by))
        || (d3 == 0 && onSegment(ax, ay, bx, by, cx, cy))
        || (d4 == 0 && onSegment(ax, ay, bx, by, dx, dy));
  }

  static bool ringTouches(std::span<const geo::LatLon> ring, double minLat, double maxLat,
                          double minLon, double maxLon) {
    size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
      double ax = ring[j].lon, ay = ring[j].lat;
      double bx = ring[i].lon, by = ring[i].lat;
      if (std::max(ax, bx) < minLon || std::min(ax, bx) > maxLon
          || std::max(ay, by) < minLat || std::min(ay, by) > maxLat) {
        continue;
      }
      if ((minLon <= ax && ax <= maxLon && minLat <= ay && ay <= maxLat)
          || (minLon <= bx && bx <= maxLon && minLat <= by && by <= maxLat)) {
        return true;
      }
      if (segmentsTouch(ax, ay, bx, by, minLon, minLat, maxLon, minLat)
          || segmentsTouch(ax, ay, bx, by, maxLon, minLat, maxLon, maxLat)
          || segmentsTouch(ax, ay, bx, by, maxLon, maxLat, minLon, maxLat)
          || segmentsTouch(ax, ay, bx, by, minLon, maxLat, minLon, minLat)) {
        return true;
      }
    }
    return false;
  }

  static bool contains(const Part& part, double lat, double lon) {
    if (lat < part.box.minLat || lat > part.box.maxLat
        || lon < part.box.minLon || lon > part.box.maxLon
        || !ringContains(part.rings.outer, lat, lon)) {
      return false;
    }
    return std::ranges::none_of(part.rings.holes, [&](std::span<const geo::LatLon> hole) {
      return ringContains(hole, lat, lon);
    });
  }

  static bool edgeTouches(const Part& part, double minLat, double maxLat,
                          double minLon, double maxLon) {
    if (ringTouches(part.rings.outer, minLat, maxLat, minLon, maxLon)) return true;
    return std::ranges::any_of(part.rings.holes, [&](std::span<const geo::LatLon> hole) {
      return ringTouches(hole, minLat, maxLat, minLon, maxLon);
    });
  }

  static BKDRelation compare(const Part& part, int32_t cellLatMin, int32_t cellLatMax,
                             int32_t cellLonMin, int32_t cellLonMax) {
    if (cellLatMax < part.latMin || part.latMax < cellLatMin
        || cellLonMax < part.lonMin || part.lonMax < cellLonMin) {
      return BKDRelation::OUTSIDE;
    }
    constexpr double MARGIN = BKDPolygonRelation::EDGE_MARGIN;
    double minLat = geo::decodeLatitude(cellLatMin);
    double maxLat = geo::decodeLatitude(cellLatMax);
    double minLon = geo::decodeLongitude(cellLonMin);
    double maxLon = geo::decodeLongitude(cellLonMax);
    if (edgeTouches(part, minLat - MARGIN, maxLat + MARGIN, minLon - MARGIN, maxLon + MARGIN)) {
      return BKDRelation::CROSSES;
    }
    return contains(part, minLat, minLon) ? BKDRelation::INSIDE : BKDRelation::OUTSIDE;
  }

public:
  explicit LinearPolygonRelation(std::span<const geo::PolygonRings> polygons) {
    for (const auto& rings : polygons) {
      geo::BoundingBox box{rings.outer[0].lat, rings.outer[0].lat,
                           rings.outer[0].lon, rings.outer[0].lon};
      for (const auto& point : rings.outer) {
        box.minLat = std::min(box.minLat, point.lat);
        box.maxLat = std::max(box.maxLat, point.lat);
        box.minLon = std::min(box.minLon, point.lon);
        box.maxLon = std::max(box.maxLon, point.lon);
      }
      parts.push_back({rings, box, geo::encodeLatitudeCeil(box.minLat),
                       geo::encodeLatitude(box.maxLat), geo::encodeLongitudeCeil(box.minLon),
                       geo::encodeLongitude(box.maxLon)});
    }
  }

  BKDRelation compare(int32_t cellLatMin, int32_t cellLatMax,
                      int32_t cellLonMin, int32_t cellLonMax) const {
    bool crosses = false;
    for (const Part& part : parts) {
      BKDRelation relation = compare(part, cellLatMin, cellLatMax, cellLonMin, cellLonMax);
      if (relation == BKDRelation::INSIDE) return relation;
      crosses |= relation == BKDRelation::CROSSES;
    }
    return crosses ? BKDRelation::CROSSES : BKDRelation::OUTSIDE;
  }

  bool matches(int32_t lat, int32_t lon) const {
    return std::ranges::any_of(parts, [&](const Part& part) {
      return part.latMin <= lat && lat <= part.latMax && part.lonMin <= lon
          && lon <= part.lonMax
          && contains(part, geo::decodeLatitude(lat), geo::decodeLongitude(lon));
    });
  }
};

} // namespace luxir::test
