// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/BooleanWeight.h"

namespace luxir {

Query::Weight* BooleanQuery::createWeight(Context& context, int32_t flags,
                                          float multiplier) {
  CompiledBoolean& plan = logicalPlan(context.planningContext());
  if (plan.singleChild != nullptr) {
    return plan.singleChild->createWeight(context, flags, multiplier);
  }
  return context.pool.make<BooleanQuery::Weight>(
      context, *this, plan, flags, multiplier);
}

} // namespace luxir
