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

#if defined(__BMI2__)
#include <immintrin.h>
#endif

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

inline void checkLatitude(double latitude) {
  if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
    throw std::invalid_argument("latitude must be finite and in [-90, 90]");
  }
}

inline void checkLongitude(double longitude) {
  if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
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

// A GEO_POINT column value is the Morton (Z-order) interleave of the two
// quantized coordinates: longitude in the odd bits, latitude in the even
// bits, each biased to unsigned first so the interleave orders like the grid.
// Longitude takes the top bit, so the unsigned interleave is bit-identical to
// a geohash's binary form (longitude bit first): the top 5p bits are the
// precision-p geohash (geohashPrefix).
// The top bit is then flipped back so signed int64 order, which column zones
// and FOR blocks use, is exactly Morton order. Nearby points share high bits,
// so a spatially clustered block packs into few bits, and a block's
// [min, max] bounds a 2-D cell (mortonCell) instead of only latitude.
inline constexpr uint64_t MORTON_ODD_BITS = UINT64_C(0xaaaaaaaaaaaaaaaa);
inline constexpr uint64_t MORTON_EVEN_BITS = UINT64_C(0x5555555555555555);
inline constexpr uint64_t MORTON_SIGN = UINT64_C(1) << 63;
inline constexpr uint32_t COORD_BIAS = UINT32_C(0x80000000);

// Deposits the 32 bits of value into the even bits of the result.
inline uint64_t spreadBits(uint32_t value) {
#if defined(__BMI2__)
  return _pdep_u64(value, MORTON_EVEN_BITS);
#else
  uint64_t x = value;
  x = (x | (x << 16)) & UINT64_C(0x0000ffff0000ffff);
  x = (x | (x << 8)) & UINT64_C(0x00ff00ff00ff00ff);
  x = (x | (x << 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
  x = (x | (x << 2)) & UINT64_C(0x3333333333333333);
  x = (x | (x << 1)) & MORTON_EVEN_BITS;
  return x;
#endif
}

// Inverse of spreadBits: gathers the even bits of value.
inline uint32_t compactBits(uint64_t value) {
#if defined(__BMI2__)
  return (uint32_t)_pext_u64(value, MORTON_EVEN_BITS);
#else
  uint64_t x = value & MORTON_EVEN_BITS;
  x = (x | (x >> 1)) & UINT64_C(0x3333333333333333);
  x = (x | (x >> 2)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
  x = (x | (x >> 4)) & UINT64_C(0x00ff00ff00ff00ff);
  x = (x | (x >> 8)) & UINT64_C(0x0000ffff0000ffff);
  x = (x | (x >> 16)) & UINT64_C(0x00000000ffffffff);
  return (uint32_t)x;
#endif
}

inline int64_t pack(int32_t latitude, int32_t longitude) {
  uint64_t morton = (spreadBits((uint32_t)longitude ^ COORD_BIAS) << 1)
      | spreadBits((uint32_t)latitude ^ COORD_BIAS);
  return (int64_t)(morton ^ MORTON_SIGN);
}

inline int64_t encodePoint(double latitude, double longitude) {
  return pack(encodeLatitude(latitude), encodeLongitude(longitude));
}

inline int32_t unpackLatitude(int64_t packed) {
  uint64_t morton = (uint64_t)packed ^ MORTON_SIGN;
  return (int32_t)(compactBits(morton) ^ COORD_BIAS);
}

inline int32_t unpackLongitude(int64_t packed) {
  uint64_t morton = (uint64_t)packed ^ MORTON_SIGN;
  return (int32_t)(compactBits(morton >> 1) ^ COORD_BIAS);
}

// The precision-p geohash cell (1 <= p <= 12) of a packed value, as the
// integer of its 5p bits: the base-32 digits of the geohash string.
inline uint64_t geohashPrefix(int64_t packed, int32_t precision) {
  return ((uint64_t)packed ^ MORTON_SIGN) >> (64 - 5 * precision);
}

// Inclusive encoded-space cell.
struct Cell {
  int32_t latMin;
  int32_t latMax;
  int32_t lonMin;
  int32_t lonMax;
};

// The smallest quadtree cell holding every packed value in [min, max]: the
// values share the Morton prefix above the highest differing bit, and each
// coordinate's remaining bits range freely.
inline Cell mortonCell(int64_t min, int64_t max) {
  uint64_t lo = (uint64_t)min ^ MORTON_SIGN;
  uint64_t hi = (uint64_t)max ^ MORTON_SIGN;
  uint64_t diff = lo ^ hi;
  uint64_t free = diff == 0 ? 0 : ~UINT64_C(0) >> std::countl_zero(diff);
  int64_t first = (int64_t)((lo & ~free) ^ MORTON_SIGN);
  int64_t last = (int64_t)((lo | free) ^ MORTON_SIGN);
  return {unpackLatitude(first), unpackLatitude(last),
          unpackLongitude(first), unpackLongitude(last)};
}

// Whether packed values in [min, max] can fall in the inclusive encoded box.
// Morton order is monotone in each coordinate, so every point of the box
// packs within [pack(latMin, lonMin), pack(latMax, lonMax)]; the range must
// also hold a value of the box's own cell, so the smallest quadtree cell of
// [min, max] must meet the box too. A reversed (dateline-wrapping) longitude
// interval falls back to the full longitude range. Conservative: an overlap
// does not imply a point in the box.
inline bool mortonRangeMayIntersect(int64_t min, int64_t max,
                                    int32_t latMin, int32_t latMax,
                                    int32_t lonMin, int32_t lonMax) {
  if (lonMax < lonMin) {
    lonMin = std::numeric_limits<int32_t>::min();
    lonMax = std::numeric_limits<int32_t>::max();
  }
  if (max < pack(latMin, lonMin) || pack(latMax, lonMax) < min) {
    return false;
  }
  Cell cell = mortonCell(min, max);
  return cell.latMin <= latMax && latMin <= cell.latMax
      && cell.lonMin <= lonMax && lonMin <= cell.lonMax;
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

// A polygon in degrees: one outer ring and optional holes. Rings are not
// closed (the last vertex connects back to the first), and edges are straight
// lines in latitude/longitude, as in Lucene's LatLonPoint polygon queries. A
// point is inside when it is inside the outer ring and inside no hole, each by
// the even-odd rule, so a self-intersecting ring still has a defined interior.
// Points exactly on an edge may fall either way.
struct Polygon {
  std::span<const LatLon> outer;
  std::span<const std::span<const LatLon>> holes;
  BoundingBox box;

  static bool ringContains(std::span<const LatLon> ring, double lat,
                           double lon) {
    bool inside = false;
    size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
      const LatLon& a = ring[i];
      const LatLon& b = ring[j];
      if ((a.lat > lat) != (b.lat > lat)
          && lon < (b.lon - a.lon) * (lat - a.lat) / (b.lat - a.lat) + a.lon) {
        inside = !inside;
      }
    }
    return inside;
  }

  bool contains(double lat, double lon) const {
    if (lat < box.minLat || lat > box.maxLat
        || lon < box.minLon || lon > box.maxLon
        || !ringContains(outer, lat, lon)) {
      return false;
    }
    for (auto hole : holes) {
      if (ringContains(hole, lat, lon)) return false;
    }
    return true;
  }

  // Whether any ring edge touches the closed rectangle. When none does, the
  // whole rectangle is on one side of every ring, so one contains() probe
  // classifies all of it.
  bool edgeTouches(double minLat, double maxLat, double minLon,
                   double maxLon) const {
    if (ringTouches(outer, minLat, maxLat, minLon, maxLon)) return true;
    for (auto hole : holes) {
      if (ringTouches(hole, minLat, maxLat, minLon, maxLon)) return true;
    }
    return false;
  }

private:
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

  static bool ringTouches(std::span<const LatLon> ring, double minLat,
                          double maxLat, double minLon, double maxLon) {
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
};

} // namespace luxir::geo
