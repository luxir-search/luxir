// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SearchOp.h"

namespace luxir {

void SearchOp::Calculator::accept(FanoutScope* tg, int32_t segment, DomainHandle domain) {
  auto* input = op.inputDomain;
  if (!input) {
    calc(tg, segment, std::move(domain));
    return;
  }
  if (segment < 0) {
    acceptAll(tg, {});
    return;
  }
  if (inputDomains) {
    inputDomains->domains[(size_t)segment] = std::move(domain);
    if (inputDomains->arrived.fetch_add(1, std::memory_order_acq_rel) + 1 == inputDomains->domains.size()) {
      acceptAll(tg, inputDomains->domains);
    }
    return;
  }
  domain = input->mapping.apply((size_t)segment, std::move(domain));
  if (!input->variants.empty()) {
    auto& leaf = op.view->segments()[(size_t)segment];
    DomainHandle reset = DomainHandle::pinned(leaf.liveDocSet(), op.view).scoped(leaf);
    if (input->applyParentFilters) reset = domain;
    else if (structural) reset = structural->prefix(segment, std::move(reset));
    DomainVariantPlan::SegmentSources sources(input->variants, *op.view, *input->planning, segment,
        op.req.memoryTracker);
    auto variants = input->variants.produceSegment(sources, std::move(domain), std::move(reset),
        op.req.memoryTracker);
    domain = std::move(variants[input->variants.variantForChild(op.name)]);
  }
  calc(tg, segment, std::move(domain));
}

void SearchOp::Calculator::acceptAll(FanoutScope* tg, std::span<const DomainHandle> domains) {
  auto* input = op.inputDomain;
  if (!input) {
    calcAll(tg, domains);
    return;
  }
  std::vector<DomainHandle> mapped;
  mapped.reserve(domains.size());
  for (size_t segment = 0; segment < domains.size(); segment++) {
    mapped.push_back(input->mapping.apply(segment, domains[segment]));
  }
  if (input->variants.empty()) {
    calcAll(tg, mapped);
    return;
  }
  auto reset = input->applyParentFilters ? mapped : structuralDomains();
  std::vector<std::unique_ptr<DomainVariantPlan::SegmentSources>> sources(mapped.size());
  auto sourcesFor = [&](size_t segment) -> DomainVariantPlan::SegmentSources& {
    auto& source = sources[segment];
    if (!source) source = std::make_unique<DomainVariantPlan::SegmentSources>(input->variants, *op.view,
        *input->planning, (int32_t)segment, op.req.memoryTracker);
    return *source;
  };
  auto variants = input->variants.produceBucketDomains(*op.view, *input->planning, mapped, reset,
      tg, &op.req, op.req.memoryTracker, sourcesFor, structuralFinish());
  size_t variant = input->variants.variantForChild(op.name);
  for (size_t segment = 0; segment < mapped.size(); segment++) mapped[segment] = std::move(variants[segment][variant]);
  calcAll(tg, mapped);
}

} // namespace luxir
