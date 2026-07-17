#pragma once

#include <cmath>
#include <stdexcept>

#include "Query.h"

namespace solux {

// Query-tree-only score multiplier. Weight creation folds the factor into
// scoring leaves, so there is deliberately no BoostQuery execution object.
class BoostQuery final : public Query {
  Query* child;
  float boost;
  bool promotesAuto;
  struct InheritedScaleTag {};

  void validate() {
    if (!std::isfinite(boost) || boost < 0.0f) {
      throw std::invalid_argument("query boost must be finite and non-negative");
    }
  }

public:
  // Public construction always represents a caller-written boost, including
  // boost 1. Boolean normalization uses inherited() for a non-promoting scale.
  BoostQuery(Query* child, float boost)
    : child(child), boost(boost), promotesAuto(true) { validate(); }

  BoostQuery(Query* child, float boost, InheritedScaleTag)
    : child(child), boost(boost), promotesAuto(false) { validate(); }

  static BoostQuery* inherited(MemPool& pool, Query* child, float boost) {
    return pool.make<BoostQuery>(child, boost, InheritedScaleTag{});
  }

  Query* getChild() const { return child; }
  float getBoost() const { return boost; }
  bool isExplicit() const { return promotesAuto; }

  ScoreProfile scoreProfile() const override {
    ScoreProfile profile = child->scoreProfile();
    if (profile.kind == ScoreProfile::Kind::VARIABLE) return profile;
    profile.value = checkedBoostProduct(profile.value, boost);
    if (promotesAuto) profile.kind = ScoreProfile::Kind::EXPLICIT_UNIFORM;
    return profile;
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    return child->createWeight(context, flags,
                               checkedBoostProduct(multiplier, boost));
  }
};

} // namespace solux
