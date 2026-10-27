// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Query.h"

namespace luxir {
class HasChildQuery final : public Query {
public:
  Query* child;
  Query* filter;
  api::HasChildQuery spec;
  HasChildQuery(Query* child, Query* filter, const api::HasChildQuery& spec)
      : Query(QueryKind::HAS_CHILD), child(child), filter(filter), spec(spec) {}
  FilterKeyScope appendFilterKey(FilterKeyBuilder&, const FilterKeyContext&) const override {
    return FilterKeyScope::UNCACHEABLE;
  }
  void validateLogicalImpl(PlanningContext& context, float multiplier = 1.0f) const override {
    child->validateLogical(context, multiplier);
    if (filter) filter->validateLogical(context, 1.0f);
  }
  Weight* createWeight(Context& context, int32_t flags, float multiplier = 1.0f) override;
};
}
