#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "solux/query/GeoQuery.h"
#include "solux/query/Query.h"
#include "solux/reader/BKDReader.h"
#include "solux/util/geo.h"

namespace solux {

// Inclusive latitude/longitude box over packed GEO_POINT column values.
// Bounds are supplied in degrees and quantized exactly like Lucene's
// LatLonPoint.newBoxQuery.
class GeoBoxQuery final : public Query {
  std::string_view field;
  int32_t minLatitude;
  int32_t maxLatitude;
  int32_t minLongitude;
  int32_t maxLongitude;
  bool empty = false;

public:
  static constexpr std::string_view QUERY_NAME = "GeoBoxQuery";
  using Weight = GeoQueryWeight<GeoBoxQuery, BKDBoxRelation>;

  GeoBoxQuery(std::string_view field, double minLat, double maxLat,
              double minLon, double maxLon) : field(field) {
    geo::checkLatitude(minLat);
    geo::checkLatitude(maxLat);
    geo::checkLongitude(minLon);
    geo::checkLongitude(maxLon);
    if (minLat > maxLat) {
      throw std::invalid_argument("minimum latitude exceeds maximum latitude");
    }

    // +90 and the non-wrapping singleton +180 are not representable after
    // quantization. Lucene returns MatchNoDocs for the same cases.
    if (minLat == 90.0 || (minLon == 180.0 && maxLon == 180.0)) {
      empty = true;
    }
    // A wrapped interval starting at +180 is equivalent to starting at -180.
    if (minLon == 180.0 && maxLon < minLon) minLon = -180.0;

    minLatitude = geo::encodeLatitudeCeil(minLat);
    maxLatitude = geo::encodeLatitude(maxLat);
    minLongitude = geo::encodeLongitudeCeil(minLon);
    maxLongitude = geo::encodeLongitude(maxLon);

    if (maxLatitude < minLatitude) empty = true;
    if (minLon <= maxLon && maxLongitude < minLongitude) empty = true;
  }

  std::string_view getField() const { return field; }
  int32_t getMinLatitude() const { return minLatitude; }
  int32_t getMaxLatitude() const { return maxLatitude; }
  int32_t getMinLongitude() const { return minLongitude; }
  int32_t getMaxLongitude() const { return maxLongitude; }
  bool isEmpty() const { return empty; }

  BKDBoxRelation makeRelation() const {
    return {minLatitude, maxLatitude, minLongitude, maxLongitude};
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    unused(multiplier);
    return context.pool.make<Weight>(context, *this, flags);
  }
};

} // namespace solux
