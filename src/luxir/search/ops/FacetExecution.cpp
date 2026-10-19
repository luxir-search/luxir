// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/FacetExecution.h"

namespace luxir {

FacetBucketBlockExecutor::InputDomainBytes FacetBucketBlockExecutor::inputDomainBytes(IndexReader& reader) {
  InputDomainBytes result;
  for (const auto& seg : reader.segments()) {
    size_t bitset = sizeof(RAMBitDocSet) + FixedBitSet::sizeInWords(seg.maxDoc()) * sizeof(uint64_t);
    size_t array = sizeof(ArrDocSet) + (size_t)DocSetBuilder::arrayLimitFor(seg.maxDoc()) * sizeof(int32_t);
    size_t bytes = saturatingAdd(std::max(bitset, array), sizeof(DomainHandle));
    result.largestSegment = std::max(result.largestSegment, bytes);
    result.wholeReader = saturatingAdd(result.wholeReader, bytes);
  }
  return result;
}

FacetBucketBlockExecutor::StructuralRoute::StructuralRoute(SearchOp::Calculator& parent,
                                                           std::span<SearchOp* const> children,
                                                           IndexReader& reader, bool hasSelection) {
  ancestor = parent.structuralConstraint();
  scopeNeeded = std::ranges::any_of(children, [](auto* child) { return child->needsStructuralScope; });
  overrides = std::ranges::any_of(children, [](auto* child) { return child->bucketDomain != nullptr; });
  selectionPath = scopeNeeded && hasSelection;
  wholeReaderPrefixes = scopeNeeded && (selectionPath || std::ranges::any_of(children, [](auto* child) {
                                          return child->needsCompleteBucketInput();
                                        }));
  bool reset = std::ranges::any_of(children, [](auto* child) {
    return child->bucketDomain && child->bucketDomain->variants.hasResetVariants();
  });
  complete = overrides && ((reset && selectionPath) || std::ranges::any_of(children, [](auto* child) {
                             return child->bucketDomain && !child->bucketDomain->variants.segmentLocal();
                           }));
  needInherited = complete && std::ranges::any_of(children, [](auto* child) {
                    if (!child->bucketDomain) return true;
                    auto& plan = child->bucketDomain->variants;
                    size_t variant = plan.variantForChild(child->name);
                    return variant == 0 || plan.frame(variant) == DomainVariantPlan::Frame::INHERIT;
                  });
  if (scopeNeeded || needInherited) {
    auto bytes = inputDomainBytes(reader);
    if (scopeNeeded) retainedBytesPerBucket = saturatingAdd(retainedBytesPerBucket,
        wholeReaderPrefixes ? bytes.wholeReader : bytes.largestSegment);
    if (needInherited) retainedBytesPerBucket = saturatingAdd(retainedBytesPerBucket, bytes.wholeReader);
  }
  if (overrides) {
    for (auto* child : children) {
      if (child->bucketDomain && std::ranges::find(plans, child->bucketDomain) == plans.end()) {
        plans.push_back(child->bucketDomain);
      }
    }
  }
}

void FacetBucketBlockExecutor::executeComplete(SearchOp::Calculator& parent,
                                               std::span<SearchOp* const> children, IndexReader& reader,
                                               size_t blockSize, std::span<BucketDomainPlan* const> plans,
                                               Bindings bindings, bool needInherited, BucketAction open,
                                               BucketAction close, BucketSource bucketDomains) {
  auto segments = reader.segments();
  auto& tracker = parent.getOp().req.memoryTracker;
  RequestMemResource resource(tracker, "bucket reset inputs", "domain variants");
  std::pmr::vector<std::pmr::vector<DomainHandle>> inputs(&resource);
  inputs.resize(blockSize);
  for (auto& row : inputs) row.resize(segments.size());
  for (size_t segnum = 0; segnum < segments.size(); segnum++) {
    bucketDomains(segnum, [&](size_t bucket, DomainHandle domain) {
      if (needInherited) inputs[bucket][segnum] = std::move(domain);
    });
  }
  std::vector<std::vector<std::unique_ptr<DomainVariantPlan::SegmentSources>>> sourceSets(plans.size());
  for (auto& sources : sourceSets) sources.resize(segments.size());
  auto releasePlans = scope_guard([&]() noexcept {
    // Complete-input selections need all segments until the block ends.
    for (auto& segment : segments) {
      for (auto* plan : plans) {
        if (std::ranges::none_of(children, [&](auto* child) {
              return child->bucketDomain == plan && child->segmentStateRetained(segment);
            }))
          plan->variants.releaseSegmentState(segment);
      }
    }
  });
  SearchOp::SegmentStateRetention retain(children, segments);
  for (size_t bucket = 0; bucket < blockSize; bucket++) {
    open(bucket);
    auto& first = bindings[bucket * children.size()];
    std::vector<DomainHandle> base;
    std::vector<std::pair<BucketDomainPlan*, std::vector<std::vector<DomainHandle>>>> routed;
    for (size_t child = 0; child < children.size(); child++) {
      auto& binding = bindings[bucket * children.size() + child];
      auto* plan = children[child]->bucketDomain;
      if (!plan) {
        binding->calcAll(nullptr, inputs[bucket]);
        continue;
      }
      if (base.empty() && plan->variants.hasResetVariants()) base = first->structuralDomains();
      auto found = std::ranges::find_if(routed, [&](const auto& entry) { return entry.first == plan; });
      if (found == routed.end()) {
        auto resetBase = base.empty() ? std::span<const DomainHandle>(inputs[bucket])
                                      : std::span<const DomainHandle>(base);
        size_t planIndex = (size_t)(std::ranges::find(plans, plan) - plans.begin());
        auto sourcesFor = [&](size_t segnum) -> DomainVariantPlan::SegmentSources& {
          auto& sources = sourceSets[planIndex][segnum];
          if (!sources)
            sources = std::make_unique<DomainVariantPlan::SegmentSources>(
                plan->variants, reader, *plan->planning, (int32_t)segnum, tracker);
          return *sources;
        };
        routed.emplace_back(plan, plan->variants.produceBucketDomains(
                                      reader, *plan->planning, inputs[bucket], resetBase, false,
                                      &parent.getOp().req, tracker, sourcesFor, first->structuralFinish()));
        found = std::prev(routed.end());
      }
      size_t variant = plan->variants.variantForChild(children[child]->name);
      std::vector<DomainHandle> domains;
      for (auto& segment : found->second) domains.push_back(segment[variant]);
      binding->calcAll(nullptr, domains);
    }
    close(bucket);
    inputs[bucket].clear();
    inputs[bucket].shrink_to_fit();
  }
}

void FacetBucketBlockExecutor::deliverStructural(SearchOp::Calculator& parent,
                                                 std::span<SearchOp* const> children, IndexReader& reader,
                                                 int32_t segnum, Bindings bindings,
                                                 const DomainHandle& domain, SegmentSources& sources) {
  std::vector<std::pair<BucketDomainPlan*, std::vector<DomainHandle>>> routed;
  for (size_t child = 0; child < children.size(); child++) {
    auto& binding = bindings[child];
    auto* plan = children[child]->bucketDomain;
    if (!plan) {
      binding->calc(nullptr, (int32_t)segnum, domain);
      continue;
    }
    auto found = std::ranges::find_if(routed, [&](const auto& entry) { return entry.first == plan; });
    if (found == routed.end()) {
      DomainHandle base = domain;
      if (plan->variants.hasResetVariants()) {
        auto& seg = reader.segments()[(size_t)segnum];
        base = DomainHandle::pinned(seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr,
                                    parent.getOp().req.reader);
        if (auto path = binding->structuralConstraint()) {
          base = path->prefix((int32_t)segnum, std::move(base));
        }
      }
      auto foundSources =
          std::ranges::find_if(sources, [&](const auto& entry) { return entry.first == plan; });
      routed.emplace_back(plan, plan->variants.produceSegment(*foundSources->second, domain, std::move(base),
                                                              parent.getOp().req.memoryTracker));
      found = std::prev(routed.end());
    }
    size_t variant = plan->variants.variantForChild(children[child]->name);
    binding->calc(nullptr, (int32_t)segnum, found->second[variant]);
  }
}

}  // namespace luxir
