#pragma once

#include <cmath>
#include <stdexcept>
#include <string_view>

#include "solux/query/GeoQuery.h"
#include "solux/query/Query.h"
#include "solux/reader/BKDReader.h"
#include "solux/util/geo.h"

namespace solux {

class GeoDistanceQuery final : public Query {
  std::string_view field;
  double centerLatitude;
  double centerLongitude;
  double radiusMeters;

public:
  static constexpr std::string_view QUERY_NAME = "GeoDistanceQuery";
  using Weight = GeoQueryWeight<GeoDistanceQuery, BKDDistanceRelation>;

  GeoDistanceQuery(std::string_view field, double centerLatitude,
                   double centerLongitude, double radiusMeters)
      : field(field), centerLatitude(centerLatitude),
        centerLongitude(centerLongitude), radiusMeters(radiusMeters) {
    geo::checkLatitude(centerLatitude);
    geo::checkLongitude(centerLongitude);
    if (!std::isfinite(radiusMeters) || radiusMeters < 0.0) {
      throw std::invalid_argument("radius must be finite and non-negative");
    }
  }

  std::string_view getField() const { return field; }
  double getCenterLatitude() const { return centerLatitude; }
  double getCenterLongitude() const { return centerLongitude; }
  double getRadiusMeters() const { return radiusMeters; }
  bool isEmpty() const { return false; }
  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  BKDDistanceRelation makeRelation() const {
    return {centerLatitude, centerLongitude, radiusMeters};
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    float score = constantWhenScored(flags, multiplier);
    return context.pool.make<Weight>(context, *this, flags, score);
  }
};

} // namespace solux
