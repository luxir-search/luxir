// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>

namespace luxir {

struct GeoPoint {
  double latitude;
  double longitude;
};

} // namespace luxir

namespace luxir::geo {

inline constexpr int16_t BITS = 32;
inline constexpr double LAT_SCALE = (double)(1ULL << BITS) / 180.0;
inline constexpr double LAT_DECODE = 1.0 / LAT_SCALE;
inline constexpr double LON_SCALE = (double)(1ULL << BITS) / 360.0;
inline constexpr double LON_DECODE = 1.0 / LON_SCALE;
inline constexpr double EARTH_MEAN_RADIUS_METERS = 6371008.7714;
// 0.2 m of slack: optimized FP codegen (contraction/FMA) can evaluate
// axisLat() up to ~0.11 m off the closed form, so 0.1 m is too tight a
// promise. Consumers only use this to widen relations, so more slack is
// strictly conservative.
inline constexpr double AXISLAT_ERROR =
    180.0 / std::numbers::pi * (0.2 / EARTH_MEAN_RADIUS_METERS);

struct BoundingBox {
  double minLat;
  double maxLat;
  double minLon;
  double maxLon;
};

inline bool validLatitude(double latitude) {
  return std::isfinite(latitude) && latitude >= -90.0 && latitude <= 90.0;
}

inline bool validLongitude(double longitude) {
  return std::isfinite(longitude) && longitude >= -180.0 && longitude <= 180.0;
}

inline void checkLatitude(double latitude) {
  if (!validLatitude(latitude)) {
    throw std::invalid_argument("latitude must be finite and in [-90, 90]");
  }
}

inline void checkLongitude(double longitude) {
  if (!validLongitude(longitude)) {
    throw std::invalid_argument("longitude must be finite and in [-180, 180]");
  }
}

// Java's (int) narrowing conversion saturates instead of being UB; the
// Lucene arithmetic relies on it. floor hits +2^31 only at exactly +90/+180,
// but ceil reaches it for ANY coordinate inside the last quantization cell
// below the maximum, so every encode goes through this.
inline int32_t saturateToInt32(double scaled) {
  if (scaled >= 2147483647.0) return std::numeric_limits<int32_t>::max();
  if (scaled <= -2147483648.0) return std::numeric_limits<int32_t>::min();
  return (int32_t)scaled;
}

inline int32_t encodeLatitude(double latitude) {
  checkLatitude(latitude);
  return saturateToInt32(std::floor(latitude / LAT_DECODE));
}

inline int32_t encodeLatitudeCeil(double latitude) {
  checkLatitude(latitude);
  return saturateToInt32(std::ceil(latitude / LAT_DECODE));
}

inline int32_t encodeLongitude(double longitude) {
  checkLongitude(longitude);
  return saturateToInt32(std::floor(longitude / LON_DECODE));
}

inline int32_t encodeLongitudeCeil(double longitude) {
  checkLongitude(longitude);
  return saturateToInt32(std::ceil(longitude / LON_DECODE));
}

inline double decodeLatitude(int32_t encoded) {
  return (double)encoded * LAT_DECODE;
}

inline double decodeLongitude(int32_t encoded) {
  return (double)encoded * LON_DECODE;
}

inline int64_t pack(int32_t latitude, int32_t longitude) {
  uint64_t bits = ((uint64_t)(uint32_t)latitude << 32) | (uint32_t)longitude;
  return (int64_t)bits;
}

inline int64_t encodePoint(double latitude, double longitude) {
  return pack(encodeLatitude(latitude), encodeLongitude(longitude));
}

inline int32_t unpackLatitude(int64_t packed) {
  return (int32_t)((uint64_t)packed >> 32);
}

inline int32_t unpackLongitude(int64_t packed) {
  return (int32_t)(uint32_t)packed;
}

// The Hilbert index of (x, y) on a 2^bits grid (Skilling, "Programming the
// Hilbert curve", AIP 2004): the axes go to transposed form, then the
// transpose's bits interleave with x leading at each level. Consecutive keys
// are neighboring cells, so sorting by key keeps nearby points together.
inline uint64_t hilbertKey(uint32_t x, uint32_t y, int bits = 32) {
  uint32_t axes[2] = {x, y};
  uint32_t top = 1u << (bits - 1);
  for (uint32_t q = top; q > 1; q >>= 1) {
    uint32_t p = q - 1;
    for (int i = 0; i < 2; i++) {
      if (axes[i] & q) {
        axes[0] ^= p;
      } else {
        uint32_t t = (axes[0] ^ axes[i]) & p;
        axes[0] ^= t;
        axes[i] ^= t;
      }
    }
  }
  axes[1] ^= axes[0];
  uint32_t t = 0;
  for (uint32_t q = top; q > 1; q >>= 1) {
    if (axes[1] & q) t ^= q - 1;
  }
  axes[0] ^= t;
  axes[1] ^= t;
  // Interleave: spread each axis to the even bits, x one place higher.
  auto spread = [](uint64_t v) {
    v = (v | (v << 16)) & 0x0000FFFF0000FFFFULL;
    v = (v | (v << 8)) & 0x00FF00FF00FF00FFULL;
    v = (v | (v << 4)) & 0x0F0F0F0F0F0F0F0FULL;
    v = (v | (v << 2)) & 0x3333333333333333ULL;
    v = (v | (v << 1)) & 0x5555555555555555ULL;
    return v;
  };
  return (spread(axes[0]) << 1) | spread(axes[1]);
}

// The 64-bit Hilbert key of a packed point (pack()): longitude as x and
// latitude as y, each biased from signed to unsigned order.
inline uint64_t hilbertKey(int64_t packed) {
  return hilbertKey((uint32_t)unpackLongitude(packed) ^ 0x80000000u,
                    (uint32_t)unpackLatitude(packed) ^ 0x80000000u);
}

// Inclusive longitude interval. A reversed interval wraps across the dateline.
// Pass B's BKD relation functor can reuse this exact encoded-space predicate.
inline bool longitudeInRange(int32_t longitude, int32_t minLongitude,
                             int32_t maxLongitude) {
  return minLongitude <= maxLongitude
      ? minLongitude <= longitude && longitude <= maxLongitude
      : longitude >= minLongitude || longitude <= maxLongitude;
}

inline double haversinSortKey(double lat1, double lon1,
                              double lat2, double lon2) {
  double x1 = lat1 * (std::numbers::pi / 180.0);
  double x2 = lat2 * (std::numbers::pi / 180.0);
  double h = (1.0 - std::cos(x1 - x2))
      + std::cos(x1) * std::cos(x2)
          * (1.0 - std::cos((lon1 - lon2) * (std::numbers::pi / 180.0)));
  uint64_t bits = std::bit_cast<uint64_t>(h) & UINT64_C(0xfffffffffffffff8);
  return std::bit_cast<double>(bits);
}

inline double haversinMeters(double sortKey) {
  return EARTH_MEAN_RADIUS_METERS * 2.0
      * std::asin(std::min(1.0, std::sqrt(sortKey * 0.5)));
}

inline double haversinMeters(double lat1, double lon1,
                             double lat2, double lon2) {
  return haversinMeters(haversinSortKey(lat1, lon1, lat2, lon2));
}

inline double distanceQuerySortKey(double radiusMeters) {
  double infiniteDistance = haversinMeters(std::numeric_limits<double>::max());
  if (radiusMeters >= infiniteDistance) return infiniteDistance;

  uint64_t lo = 0;
  uint64_t hi = std::bit_cast<uint64_t>(std::numeric_limits<double>::max());
  while (lo <= hi) {
    uint64_t mid = lo + ((hi - lo) >> 1);
    double sortKey = std::bit_cast<double>(mid);
    double midRadius = haversinMeters(sortKey);
    if (midRadius == radiusMeters) return sortKey;
    if (midRadius > radiusMeters) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return std::bit_cast<double>(lo);
}

inline double axisLat(double centerLat, double radiusMeters) {
  constexpr double HALF_PI = std::numbers::pi / 2.0;
  double l1 = centerLat * (std::numbers::pi / 180.0);
  double radius = (radiusMeters + 7e-2) / EARTH_MEAN_RADIUS_METERS;
  if (std::abs(l1) + radius >= HALF_PI) {
    return centerLat >= 0.0 ? 90.0 : -90.0;
  }
  l1 = centerLat >= 0.0 ? HALF_PI - l1 : l1 + HALF_PI;
  double l2 = std::acos(std::clamp(std::cos(l1) / std::cos(radius),
                                  -1.0, 1.0));
  l2 = centerLat >= 0.0 ? HALF_PI - l2 : l2 - HALF_PI;
  return l2 * (180.0 / std::numbers::pi);
}

inline BoundingBox circleBoundingBox(double centerLat, double centerLon,
                                     double radiusMeters) {
  checkLatitude(centerLat);
  checkLongitude(centerLon);
  if (!std::isfinite(radiusMeters) || radiusMeters < 0.0) {
    throw std::invalid_argument("radius must be finite and non-negative");
  }

  constexpr double HALF_PI = std::numbers::pi / 2.0;
  double radLat = centerLat * (std::numbers::pi / 180.0);
  double radLon = centerLon * (std::numbers::pi / 180.0);
  double radDistance = (radiusMeters + 7e-2) / EARTH_MEAN_RADIUS_METERS;
  double minLat = radLat - radDistance;
  double maxLat = radLat + radDistance;
  double minLon;
  double maxLon;
  if (minLat > -HALF_PI && maxLat < HALF_PI) {
    double deltaLon = std::asin(std::clamp(
        std::sin(radDistance) / std::cos(radLat), -1.0, 1.0));
    minLon = radLon - deltaLon;
    if (minLon < -std::numbers::pi) minLon += 2.0 * std::numbers::pi;
    maxLon = radLon + deltaLon;
    if (maxLon > std::numbers::pi) maxLon -= 2.0 * std::numbers::pi;
  } else {
    minLat = std::max(minLat, -HALF_PI);
    maxLat = std::min(maxLat, HALF_PI);
    minLon = -std::numbers::pi;
    maxLon = std::numbers::pi;
  }
  return {minLat * (180.0 / std::numbers::pi),
          maxLat * (180.0 / std::numbers::pi),
          minLon * (180.0 / std::numbers::pi),
          maxLon * (180.0 / std::numbers::pi)};
}

struct LatLon {
  double lat;
  double lon;
};

// One polygon's rings: the outer ring and optional holes, each a vertex list
// whose last vertex connects back to the first.
struct PolygonRings {
  std::span<const LatLon> outer;
  std::span<const std::span<const LatLon>> holes;
};

// One ring edge, from the previous vertex (lat1, lon1) to the next one
// (lat2, lon2). low is the edge's lower latitude and max the highest latitude
// of the edge and of its subtree in the EdgeTree.
struct Edge {
  double lat1;
  double lon1;
  double lat2;
  double lon2;
  double low;
  double max;
};

// A ring's edges as Lucene's EdgeTree: an interval tree over latitude, kept
// implicit in an array sorted by (low, max) whose middle element roots each
// range. A walk skips every subtree whose latitudes cannot meet the query, so
// a point or a box visits O(log V + k) edges for k edges spanning its
// latitude, not all V. Each visited edge gets the same test a scan of the
// ring would give it, so the answers are the scan's.
struct EdgeTree {
  std::span<const Edge> edges;

  // Fills edges, one per ring vertex, and returns the tree over them.
  static EdgeTree build(std::span<const LatLon> ring, std::span<Edge> edges) {
    size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
      const LatLon& a = ring[j];
      const LatLon& b = ring[i];
      edges[i] = {a.lat, a.lon, b.lat, b.lon, std::min(a.lat, b.lat),
                  std::max(a.lat, b.lat)};
    }
    std::ranges::sort(edges, [](const Edge& x, const Edge& y) {
      return x.low != y.low ? x.low < y.low : x.max < y.max;
    });
    pullMax(edges);
    return {edges};
  }

  // Even-odd: whether a ray from the point toward greater longitude crosses
  // the ring an odd number of times.
  bool contains(double lat, double lon) const {
    return crossesOdd(edges, lat, lon);
  }

  // Whether any edge touches the closed rectangle.
  bool touches(double minLat, double maxLat, double minLon,
               double maxLon) const {
    return anyTouches(edges, minLat, maxLat, minLon, maxLon);
  }

private:
  static size_t root(std::span<const Edge> range) {
    return (range.size() - 1) / 2;
  }

  static double pullMax(std::span<Edge> range) {
    if (range.empty()) return -std::numeric_limits<double>::infinity();
    size_t mid = root(range);
    Edge& node = range[mid];
    node.max = std::max({node.max, pullMax(range.first(mid)),
                         pullMax(range.subspan(mid + 1))});
    return node.max;
  }

  // An edge flips the ray's parity when its latitudes straddle lat, so lat
  // lies in [low, highest latitude): subtrees with max <= lat cannot flip,
  // and no edge right of a node with low > lat can either.
  static bool crossesOdd(std::span<const Edge> range, double lat, double lon) {
    bool odd = false;
    while (!range.empty()) {
      size_t mid = root(range);
      const Edge& edge = range[mid];
      if (lat >= edge.max) break;
      if ((edge.lat2 > lat) != (edge.lat1 > lat)
          && lon < (edge.lon1 - edge.lon2) * (lat - edge.lat2)
                       / (edge.lat1 - edge.lat2) + edge.lon2) {
        odd = !odd;
      }
      odd ^= crossesOdd(range.first(mid), lat, lon);
      if (lat < edge.low) break;
      range = range.subspan(mid + 1);
    }
    return odd;
  }

  static bool anyTouches(std::span<const Edge> range, double minLat,
                         double maxLat, double minLon, double maxLon) {
    while (!range.empty()) {
      size_t mid = root(range);
      const Edge& edge = range[mid];
      if (minLat > edge.max) return false;
      if (edgeTouches(edge, minLat, maxLat, minLon, maxLon)
          || anyTouches(range.first(mid), minLat, maxLat, minLon, maxLon)) {
        return true;
      }
      if (maxLat < edge.low) return false;
      range = range.subspan(mid + 1);
    }
    return false;
  }

  static double orient(double ax, double ay, double bx, double by,
                       double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  }

  // Closed segment intersection; collinear overlaps count.
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
    auto onSegment = [](double px, double py, double qx, double qy,
                        double rx, double ry) {
      return std::min(px, qx) <= rx && rx <= std::max(px, qx)
          && std::min(py, qy) <= ry && ry <= std::max(py, qy);
    };
    return (d1 == 0 && onSegment(cx, cy, dx, dy, ax, ay))
        || (d2 == 0 && onSegment(cx, cy, dx, dy, bx, by))
        || (d3 == 0 && onSegment(ax, ay, bx, by, cx, cy))
        || (d4 == 0 && onSegment(ax, ay, bx, by, dx, dy));
  }

  static bool edgeTouches(const Edge& edge, double minLat, double maxLat,
                          double minLon, double maxLon) {
    double ax = edge.lon1, ay = edge.lat1;
    double bx = edge.lon2, by = edge.lat2;
    if (std::max(ax, bx) < minLon || std::min(ax, bx) > maxLon
        || std::max(ay, by) < minLat || std::min(ay, by) > maxLat) {
      return false;
    }
    if ((minLon <= ax && ax <= maxLon && minLat <= ay && ay <= maxLat)
        || (minLon <= bx && bx <= maxLon && minLat <= by && by <= maxLat)) {
      return true;
    }
    return segmentsTouch(ax, ay, bx, by, minLon, minLat, maxLon, minLat)
        || segmentsTouch(ax, ay, bx, by, maxLon, minLat, maxLon, maxLat)
        || segmentsTouch(ax, ay, bx, by, maxLon, maxLat, minLon, maxLat)
        || segmentsTouch(ax, ay, bx, by, minLon, maxLat, minLon, minLat);
  }
};

// A polygon in degrees: one outer ring and optional holes. Edges are
// straight lines in latitude/longitude, as in Lucene's LatLonPoint polygon
// queries. A point is inside when it is inside the outer ring and inside no
// hole, each by the even-odd rule, so a self-intersecting ring still has a
// defined interior. Points exactly on an edge may fall either way.
struct Polygon {
  EdgeTree outer;
  std::span<const EdgeTree> holes;
  BoundingBox box;

  // The polygon of the trees, bounded by its outer ring's vertices.
  static Polygon of(EdgeTree outer, std::span<const EdgeTree> holes) {
    const Edge& first = outer.edges[0];
    BoundingBox box{first.lat1, first.lat1, first.lon1, first.lon1};
    for (const Edge& edge : outer.edges) {
      box.minLat = std::min(box.minLat, edge.lat1);
      box.maxLat = std::max(box.maxLat, edge.lat1);
      box.minLon = std::min(box.minLon, edge.lon1);
      box.maxLon = std::max(box.maxLon, edge.lon1);
    }
    return {outer, holes, box};
  }

  bool contains(double lat, double lon) const {
    if (lat < box.minLat || lat > box.maxLat
        || lon < box.minLon || lon > box.maxLon
        || !outer.contains(lat, lon)) {
      return false;
    }
    for (const EdgeTree& hole : holes) {
      if (hole.contains(lat, lon)) return false;
    }
    return true;
  }

  // Whether any ring edge touches the closed rectangle. When none does, the
  // whole rectangle is on one side of every ring, so one contains() probe
  // classifies all of it.
  bool edgeTouches(double minLat, double maxLat, double minLon,
                   double maxLon) const {
    if (outer.touches(minLat, maxLat, minLon, maxLon)) return true;
    for (const EdgeTree& hole : holes) {
      if (hole.touches(minLat, maxLat, minLon, maxLon)) return true;
    }
    return false;
  }
};

} // namespace luxir::geo
