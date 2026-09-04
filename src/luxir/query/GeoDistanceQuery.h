#pragma once

#include <cmath>
#include <stdexcept>
#include <string_view>

#include "luxir/query/GeoQuery.h"
#include "luxir/query/Query.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/util/geo.h"

namespace luxir {

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
      : Query(QueryKind::GEO_DISTANCE), field(field),
        centerLatitude(centerLatitude),
        centerLongitude(centerLongitude), radiusMeters(radiusMeters) {
    geo::checkLatitude(centerLatitude);
    geo::checkLongitude(centerLongitude);
    if (!std::isfinite(radiusMeters) || radiusMeters < 0.0) {
      throw std::invalid_argument("radius must be finite and non-negative");
    }
  }

  bool equals(const Query& other) const override {
    if (other.getKind() != kind) return false;
    const auto& rhs = static_cast<const GeoDistanceQuery&>(other);
    return field == rhs.field
        && std::bit_cast<uint64_t>(centerLatitude)
            == std::bit_cast<uint64_t>(rhs.centerLatitude)
        && std::bit_cast<uint64_t>(centerLongitude)
            == std::bit_cast<uint64_t>(rhs.centerLongitude)
        && std::bit_cast<uint64_t>(radiusMeters)
            == std::bit_cast<uint64_t>(rhs.radiusMeters);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, std::bit_cast<uint64_t>(centerLatitude));
    value = mixHash(value, std::bit_cast<uint64_t>(centerLongitude));
    return mixHash(value, std::bit_cast<uint64_t>(radiusMeters));
  }

  std::string_view getField() const { return field; }
  double getCenterLatitude() const { return centerLatitude; }
  double getCenterLongitude() const { return centerLongitude; }
  double getRadiusMeters() const { return radiusMeters; }
  bool isEmpty() const { return false; }
  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  bool canOmitWeightForCacheFirstMembership() const override { return true; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendDouble(centerLatitude);
    out.appendDouble(centerLongitude);
    out.appendDouble(radiusMeters);
    return FilterKeyScope::SEGMENT_STABLE;
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

} // namespace luxir
