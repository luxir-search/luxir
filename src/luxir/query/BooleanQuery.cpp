// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/BooleanWeight.h"

namespace luxir {

Query::Weight* BooleanQuery::createWeight(Context& context, int32_t flags,
                                          float multiplier) {
  return dispatchWeight(context.planningContext(), [&](Query& child) {
    return child.buildWeight(context, flags, multiplier);
  }, [&](CompiledBoolean& plan) -> Query::Weight* {
    return context.pool.make<BooleanQuery::Weight>(
        context, *this, plan, flags, multiplier, ScorePolicy{});
  });
}

Query::Weight* BooleanQuery::makeDisjunctionWeight(
    Context& context, const Query& owner, std::span<Query*> clauses,
    ScorePolicy policy, int32_t flags, float multiplier) {
  assert(!clauses.empty());
  CompiledBoolean plan;
  plan.optional = clauses;
  plan.minShouldMatch = 1;
  return context.pool.make<BooleanQuery::Weight>(
      context, owner, plan, flags, multiplier, policy);
}

} // namespace luxir
