// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/SearchOp.h"

namespace luxir {

StructuralConstraint::Producer::Producer(SearchRequest& req, const StructuralConstraint* ancestor,
                                         size_t bucketCount)
    : sourcesResource(req.memoryTracker, "structural sources", "domain variants"),
      sources(&sourcesResource),
      req(req),
      ancestor(ancestor),
      bucketCount(bucketCount),
      memo(&sourcesResource),
      ready(&sourcesResource) {
  memo.resize(req.reader->segments().size());
  ready.resize(memo.size());
}

DomainVariantPlan::SegmentSources& StructuralConstraint::Producer::segmentSources(
    const DomainVariantPlan& plan, IndexReader& reader, Query::PlanningContext& planning, int32_t segment,
    RequestMemTracker& tracker) {
  auto found = std::ranges::find_if(
      sources, [&](const auto& entry) { return entry.identity == &plan && entry.segment == segment; });
  if (found == sources.end()) {
    sources.push_back(
        {&plan, segment,
         std::make_unique<DomainVariantPlan::SegmentSources>(plan, reader, planning, segment, tracker)});
    found = std::prev(sources.end());
  }
  return *found->values;
}

void StructuralConstraint::Producer::releaseSources(int32_t segment) {
  std::erase_if(sources, [&](const auto& entry) { return entry.segment == segment; });
}

DomainVariantPlan::SegmentSources& StructuralConstraint::Producer::filterSources(
    std::span<Query::Weight* const> weights, std::span<FilterCache::Use* const> uses, IndexReader& reader,
    Query::PlanningContext& planning, int32_t segment, RequestMemTracker& tracker) {
  auto found = std::ranges::find_if(sources, [&](const auto& entry) {
    return entry.identity == weights.data() && entry.segment == segment;
  });
  if (found == sources.end()) {
    sources.push_back({weights.data(), segment,
                       std::make_unique<DomainVariantPlan::SegmentSources>(weights, uses, reader, planning,
                                                                           segment, tracker)});
    found = std::prev(sources.end());
  }
  return *found->values;
}

bool StructuralConstraint::hasSelection() const {
  return selection != nullptr || (parent && parent->hasSelection());
}

DomainHandle StructuralConstraint::prefix(int32_t segment, DomainHandle eligibility) const {
  if (hasSelection()) return parent ? parent->prefix(segment, std::move(eligibility)) : eligibility;
  return producer->prefix(segment, bucket);
}

std::vector<DomainHandle> StructuralConstraint::finish(IndexReader& reader, std::vector<DomainHandle> domains,
                                                       ExecutionWarnings* warnings,
                                                       RequestMemTracker& tracker) const {
  if (parent) domains = parent->finish(reader, std::move(domains), warnings, tracker);
  if (!hasSelection()) return domains;  // The prefix is already applied.
  if (!selection) {
    for (size_t segment = 0; segment < domains.size(); segment++) {
      domains[segment] = producer->recover((int32_t)segment, bucket, std::move(domains[segment]));
    }
    return domains;
  }
  // Fresh preparation within this reset's scope, outer selection first.
  // No FilterUse: the selected window is derived request-local state.
  std::vector<DocSet*> views;
  for (const auto& domain : domains) views.push_back(domain.get());
  Query::Weight::PrepareContext context{reader, std::span<DocSet* const>(views.data(), views.size()), false,
                                        warnings};
  auto* weight = selection;
  auto prepared = weight->prepare(context);
  DomainVariantPlan::Reservation preparedCharge(tracker, prepared ? prepared->retainedBytes() : 0,
                                                "structural selection window");
  // Destroy only this prepared window. The shared bucket Weight (including
  // its child state pools) is released by its owner's retention protocol.
  // Prepared sources may borrow their complete input, so keep it intact
  // through the last segment and prepared-state release.
  std::vector<DomainHandle> selected;
  selected.reserve(domains.size());
  for (size_t segment = 0; segment < domains.size(); segment++) {
    auto docs =
        QueryPrep::materialize(*weight, prepared.get(), reader.segments()[segment], domains[segment].get());
    auto charge = std::make_shared<DomainVariantPlan::Reservation>(tracker, docs ? docs->ramBytesUsed() : 0,
                                                                   "structural selection domain");
    selected.push_back(DomainHandle(std::move(docs)).chargedWith(std::move(charge)));
  }
  return selected;
}

std::vector<DomainHandle> SearchOp::Calculator::structuralDomains() const {
  std::vector<DomainHandle> domains;
  auto path = structuralConstraint();
  for (auto& segment : op.req.reader->segments()) {
    DomainHandle live =
        DomainHandle::pinned(segment.liveDocs() ? &segment.liveDocs()->docset() : nullptr, op.req.reader);
    domains.push_back(path ? path->prefix(segment.ord, std::move(live)) : std::move(live));
  }
  return domains;
}

StructuralConstraint::Finish SearchOp::Calculator::structuralFinish() const {
  auto path = structuralConstraint();
  if (!path || !path->hasSelection()) return {};
  return [this, path = std::move(path)](std::vector<DomainHandle> domains) {
    return path->finish(*op.req.reader, std::move(domains), &op.req, op.req.memoryTracker);
  };
}

void StructuralConstraint::Producer::releaseSegment(int32_t segnum) {
  memo[(size_t)segnum].clear();
  memo[(size_t)segnum].shrink_to_fit();
  ready[(size_t)segnum] = 2;
  releaseSources(segnum);
}
DomainHandle StructuralConstraint::Producer::prefix(int32_t segnum, size_t bucket) {
  auto& values = memo[(size_t)segnum];
  if (ready[(size_t)segnum] == 2 && structuralPrefixRecomputations != nullptr) {
    (*structuralPrefixRecomputations)++;
  }
  if (ready[(size_t)segnum] != 1) {
    auto& seg = req.reader->segments()[(size_t)segnum];
    DomainHandle base =
        DomainHandle::pinned(seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr, req.reader);
    if (ancestor) base = ancestor->prefix(segnum, std::move(base));
    values.resize(bucketCount);
    if (base.get() && base.get()->card() == 0) {
      // No descendant identity can add documents to an empty prefix.
      // In particular, avoid traversing a wide points range here.
      std::fill(values.begin(), values.end(), base);
    } else {
      [[maybe_unused]] size_t fed = 0;
      produce((size_t)segnum, 0, bucketCount, base, Feed([&](size_t index, DomainHandle domain) {
                assert(index == fed++);
                assert(index < values.size());
                assert(domain.isDeliverable());
                assert(!base.get() || domain.get());
                values[index] = domain.get() == base.get()
                                    ? std::move(domain)
                                    : DomainVariantPlan::account(std::move(domain), req.memoryTracker,
                                                                 "structural bucket domain");
              }));
      assert(fed == bucketCount);
    }
    ready[(size_t)segnum] = 1;
  }
  return values[bucket];
}
DomainHandle StructuralConstraint::Producer::recover(int32_t segnum, size_t bucket,
                                                     DomainHandle eligibility) {
  if (eligibility.get() && eligibility.get()->card() == 0) return eligibility;
  DomainHandle result;
  [[maybe_unused]] size_t fed = 0;
  produce((size_t)segnum, bucket, 1, eligibility, Feed([&](size_t index, DomainHandle domain) {
            assert(index == fed++);
            assert(domain.isDeliverable());
            assert(!eligibility.get() || domain.get());
            result = domain.get() == eligibility.get()
                         ? std::move(domain)
                         : DomainVariantPlan::account(std::move(domain), req.memoryTracker,
                                                      "structural bucket domain");
          }));
  assert(fed == 1);
  return result;
}

}  // namespace luxir
