// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <utility>

#include "luxir/search/ops/FacetOp.h"
#include "luxir/search/ops/FusionOp.h"
#include "luxir/search/ops/RootOp.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

void RootOp::materialize() {
  if (domainVariants.empty()) return;
  weightContext = Query::Context::create(&req.arena, *planning);
  domainVariants.buildWeights(*weightContext, *planning, 0);
}

void BucketDomainPlan::materialize() {
  auto* context = Query::Context::create(&arena, *planning);
  variants.buildWeights(*context, *planning, 0);
}

void InputDomainPlan::materialize() {
  auto* context = Query::Context::create(&arena, *planning);
  variants.buildWeights(*context, *planning, 0);
}

Query::Context& TopDocsReq::decoratorQueryContext() {
  if (weightContext == nullptr) {
    weightContext = Query::Context::create(&req.arena, planning);
    skipCount(SkipStats::queryContextsCreated);
  }
  return *weightContext;
}

void TopDocsReq::bindDecorators() {
  HitDecorator::QuerySource bound{resultQuery, decoratorQueryContext()};
  for (const auto& decorator : decorators) decorator->bind({&bound, 1});
}

void TopDocsReq::validateRowExecution() const {
  if (weight->needsPrepare() || !filterWeights.empty()) {
    throw RequestError("row operation '" + std::string(name) + "': prepared queries and routed filters are not supported yet");
  }
}

void QueryFacetReq::materialize() {
  auto* context = Query::Context::create(&req.arena, planning);
  auto weights = req.requestPool.make_span<Query::Weight*>(bucketQueries.size());
  for (size_t i = 0; i < weights.size(); i++) {
    weights[i] = context->createRootWeight(*bucketQueries[i], 0);
  }
  std::span<FilterCache::Use*> uses;
  if (bucketChildInPlanningView) {
    uses = req.requestPool.make_span<FilterCache::Use*>(weights.size());
    for (size_t i = 0; i < weights.size(); i++) {
      uses[i] = !weights[i]->matchesAllDocs() && !weights[i]->needsPrepare()
              && bucketQueries[i]->recoverableAsPredicate()
          ? planning.getFilterUse(*bucketQueries[i]) : nullptr;
    }
  }
  bucketWeights = weights;
  bucketUses = uses;
  preparedBuckets = QueryPrep::anyNeedsPrepare(bucketWeights);
  assert(bucketWeights.size() == queryFacet.buckets.size());
  assert(bucketUses.empty() || bucketUses.size() == bucketWeights.size());
}

void FusionOp::materialize() {
  if (filters.empty()) return;
  assert(!sources.empty());
  auto& planning = sources.front()->planning;
  // A fully resident source may have skipped context creation.
  auto* sharedContext = sources.front()->weightContext;
  if (sharedContext == nullptr) {
    sharedContext = Query::Context::create(&req.arena, planning);
    skipCount(SkipStats::queryContextsCreated);
  }
  {
    auto& opName = planning.opName;
    auto savedName = std::exchange(opName, name);
    auto restoreName = scope_guard([&]() noexcept { opName = savedName; });
    filterWeights = buildFilterWeights(filters, *sharedContext, Query::NEED_SCORES);
  }
  assert(filterWeights.size() == filters.size());
  auto uses = planning.pool.make_span<FilterCache::Use*>(filters.size());
  for (size_t i = 0; i < filters.size(); i++) {
    uses[i] = planning.getFilterUse(*filters[i].query);
  }
  filterUses = uses;
}

void FusionOp::bindDecorators() {
  std::vector<HitDecorator::QuerySource> boundQueries;
  for (auto* source : sources) {
    boundQueries.push_back({source->resultQuery, source->decoratorQueryContext()});
  }
  for (const auto& decorator : decorators) decorator->bind(boundQueries);
}

}  // namespace luxir
