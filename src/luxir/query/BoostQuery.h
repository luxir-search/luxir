#pragma once

#include <cmath>
#include <stdexcept>

#include "Query.h"

namespace luxir {

// Query-tree-only score multiplier. Weight creation folds the factor into
// scoring leaves, so there is deliberately no BoostQuery execution object.
class BoostQuery final : public Query {
  Query* child;
  float boost;

public:
  BoostQuery(Query* child, float boost)
    : Query(QueryKind::BOOST), child(child), boost(boost) {
    if (!std::isfinite(boost) || boost < 0.0f) {
      throw std::invalid_argument("query boost must be finite and non-negative");
    }
  }

  Query* getChild() const { return child; }
  float getBoost() const { return boost; }

  FieldSortConjunction fieldSortConjunction(
      PlanningContext& context) const override {
    return child->fieldSortConjunction(context);
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    child->validateLogical(
        context, checkedBoostProduct(multiplier, boost));
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    return child->appendFilterKey(out, ctx);
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    return child->createWeight(context, flags, multiplier * boost);
  }
};

} // namespace luxir
