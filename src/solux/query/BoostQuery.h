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

public:
  BoostQuery(Query* child, float boost) : child(child), boost(boost) {
    if (!std::isfinite(boost) || boost < 0.0f) {
      throw std::invalid_argument("query boost must be finite and non-negative");
    }
  }

  Query* getChild() const { return child; }
  float getBoost() const { return boost; }

  // A boost is a pure multiplier: it scales the child's uniform value but
  // never changes its kind, so a boosted automatic constant is still
  // suppressed in required position. constant_score (^=) is the opt-in for
  // a constant that scores everywhere.
  ScoreProfile scoreProfile() const override {
    ScoreProfile profile = child->scoreProfile();
    if (profile.kind == ScoreProfile::Kind::VARIABLE) return profile;
    profile.value = checkedBoostProduct(profile.value, boost);
    return profile;
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    return child->createWeight(context, flags,
                               checkedBoostProduct(multiplier, boost));
  }
};

} // namespace solux
