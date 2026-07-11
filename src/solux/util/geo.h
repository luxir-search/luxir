#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace solux::geo {

inline constexpr int16_t BITS = 32;
inline constexpr double LAT_SCALE = (double)(1ULL << BITS) / 180.0;
inline constexpr double LAT_DECODE = 1.0 / LAT_SCALE;
inline constexpr double LON_SCALE = (double)(1ULL << BITS) / 360.0;
inline constexpr double LON_DECODE = 1.0 / LON_SCALE;

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

} // namespace solux::geo
