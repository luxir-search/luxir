// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/QueryPrep.h"
#include "luxir/search/Collector.h"

namespace luxir::QueryPrep {

int64_t countSegment(MemPool& pool, Query::ScorerSupplier* supplier,
                     DocSet* domain, int32_t maxDoc) {
  if (supplier == nullptr) {
    skipCount(SkipStats::scalarCountConstants);
    return 0;
  }
  Query::ScorerSupplier::BulkScorerContext bulkContext;
  auto plan = supplier->planBulk(Query::ExecutionUse::COUNT_WINDOWS, bulkContext);
  if (plan.available == Query::ScorerSupplier::BulkAnswer::YES) {
    if (plan.hasConstantCount()) {
      if (domain == nullptr) {
        skipCount(SkipStats::scalarCountConstants);
        return plan.constantCount;
      }
    } else if (auto* bulk = supplier->buildBulk(pool, plan)) {
      skipCount(SkipStats::scalarCountWindows);
      return countMatchesWindowed(bulk, domain, nullptr, maxDoc);
    }
  } else {
    supplier->recordBulkPlanCommitment(
        Query::ExecutionUse::COUNT_WINDOWS, bulkContext, plan);
  }
  skipCount(SkipStats::scalarCountPulls);
  auto demand = Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max());
  auto* pull = supplier->resolve(pool, supplier->makePlanContext(demand));
  auto* scorer = pull->build(pool);
  return scorer ? countMatchesInRange(scorer, domain, 0, maxDoc) : 0;
}

DocSet* populateRawFilter(Query::Weight& weight, Query::Weight::PreparedWeight* prepared,
                          IndexReader::Segment& segment, FilterCache::Use& use,
                          FilterCache::Probe& probe) {
  using Kind = FilterCache::Probe::Kind;
  assert(probe.kind() != Kind::HIT);

  size_t ord = (size_t)segment.ord;
  if (probe.kind() == Kind::BUILD) {
    auto buildStart = std::chrono::steady_clock::now();
    auto raw = materializeRawFilter(weight, prepared, segment);
    use.publishRawByproduct(ord, probe, std::move(raw), elapsedBuildNanos(buildStart));
    return use.rawDocSet(ord);
  }

  return use.adoptOwnedRaw(ord, probe, materializeRawFilter(weight, prepared, segment));
}

}  // namespace luxir::QueryPrep
