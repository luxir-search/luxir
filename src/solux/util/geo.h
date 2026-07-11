#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace solux::geo {

inline constexpr int16_t BITS = 32;
inline constexpr double LAT_SCALE = (double)(1ULL << BITS) / 180.0;
inline constexpr double LAT_DECODE = 1.0 / LAT_SCALE;
inline constexpr double LON_SCALE = (double)(1ULL << BITS) / 360.0;
inline constexpr double LON_DECODE = 1.0 / LON_SCALE;
inline constexpr double EARTH_MEAN_RADIUS_METERS = 6371008.7714;
inline constexpr double AXISLAT_ERROR =
    180.0 / std::numbers::pi * (0.1 / EARTH_MEAN_RADIUS_METERS);

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

} // namespace solux::geo
