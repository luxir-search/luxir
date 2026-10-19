// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/TopDocsReq.h"

namespace luxir {

// Anchor calculator vtables and collection instantiations outside the parser
// translation unit, where they would share every op's inlining budget.
SearchOp::Calculator* TopDocsReq::createCalculator(
    Calculator* parent, int64_t slot, int64_t numSlots,
    const StructuralConstraint* structural) {
  auto* scope = structural ? structural : parent ? parent->structuralConstraint() : nullptr;
  if (diversify) return createDiversifyCalculator(parent, slot, numSlots, scope);
  if (metricOrder) {
    return createMetricGroupCalculator(*this, parent, slot, numSlots, scope);
  }
  if (scope && domainVariants.hasResetVariants()) {
    return new Calc<true>(*this, parent, slot, numSlots, scope);
  }
  return new Calc<false>(*this, parent, slot, numSlots, scope);
}

}  // namespace luxir
