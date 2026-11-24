// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/TopDocsReq.h"

namespace luxir {

// Each variant owes an independent output. A reset starts from its own
// source even when the input or the default result is empty.
bool TopDocsReq::outputsEmpty(IndexReader::Segment& segment, DocSet* input,
                              std::vector<std::string>* details) const {
  auto note = [&](std::string_view reason) {
    if (details) details->push_back(std::format("op {}: {}", name, reason));
  };
  auto excludes = [&](const Query::Weight* source) {
    if (!source || !source->provenEmpty(segment)) return false;
    if (details) note(source->proofReason(segment));
    return true;
  };
  if (segment.liveCountBounds().second == 0) {
    note("live_docs=0");
    return true;
  }
  auto empty = [&](size_t variant) {
    if (domainVariants.frame(variant) == DomainVariantPlan::Frame::RESET) {
      if (excludes(domainVariants.sourceWeight(domainVariants.resetSource(variant)))) return true;
    } else {
      if (excludes(weight)) return true;
      if (input && input->cachedCard() == 0) {
        note("incoming domain empty");
        return true;
      }
    }
    size_t offset = 0;
    if (!filterWeights.empty()) for (size_t i = 0; i < filters.size(); ++i) {
      auto sources = filterWeights.subspan(offset, filters[i].sourceCount());
      offset += sources.size();
      if (variant && !domainVariants.includesFilter(variant, i)) continue;
      if (std::ranges::all_of(sources, [&](auto* w) { return w->provenEmpty(segment); })) {
        if (details) for (auto* w : sources) note(w->proofReason(segment));
        return true;
      }
    }
    for (size_t source : domainVariants.extraSources(variant)) {
      if (excludes(domainVariants.sourceWeight(source))) return true;
    }
    return false;
  };
  for (size_t variant = 0; variant < domainVariants.variantCount(); ++variant) {
    if (!empty(variant)) return false;
  }
  return true;
}

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
