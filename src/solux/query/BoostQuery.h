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

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    return child->createWeight(context, flags,
                               checkedBoostProduct(multiplier, boost));
  }
};

} // namespace solux
