// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/QueryPrep.h"
#include "luxir/search/Collector.h"

namespace luxir::QueryPrep {

int64_t countSegment(MemPool& pool, Query::ScorerSupplier* supplier,
                     DocSet* domain, int32_t maxDoc) {
  if (supplier == nullptr) return 0;
  Query::ScorerSupplier::BulkScorerContext bulkContext;
  auto plan = supplier->planBulk(Query::ExecutionUse::COUNT_WINDOWS, bulkContext);
  if (plan.available == Query::ScorerSupplier::BulkAnswer::YES) {
    if (plan.hasConstantCount()) {
      if (domain == nullptr) return plan.constantCount;
    } else if (auto* bulk = supplier->buildBulk(pool, plan)) {
      return countMatchesWindowed(bulk, domain, nullptr, maxDoc);
    }
  } else {
    supplier->recordBulkPlanCommitment(
        Query::ExecutionUse::COUNT_WINDOWS, bulkContext, plan);
  }
  auto demand = Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max());
  auto* pull = supplier->resolve(pool, supplier->makePlanContext(demand));
  auto* scorer = pull->build(pool);
  return scorer ? countMatchesInRange(scorer, domain, 0, maxDoc) : 0;
}

DomainHandle materializeEffectiveFilter(const PreparedSource& source, IndexReader& reader,
                                        IndexReader::Segment& segment, DocSet* domain,
                                        FilterCache::Probe probe) {
  auto& weight = *source.weight;
  auto* prepared = source.prepared.get();
  auto* use = source.cacheUse;
  if (source.domainDependence != PreparedDomainDependence::QUERY_CANONICAL && use != nullptr &&
      use->scope() != FilterKeyScope::READER_STABLE) {
    use = nullptr;
  }
  if (use == nullptr) {
    return DomainHandle(materialize(weight, prepared, segment, domain)).scoped(segment);
  }

  if (use->scope() == FilterKeyScope::READER_STABLE) {
    if (auto* cached = dynamic_cast<ReaderStablePreparedWeight*>(prepared)) {
      [[maybe_unused]] DocSet* canonical = segment.liveDocSet();
      assert(domain == nullptr || domain == canonical);
      return DomainHandle::borrowed(cached->docSet(segment)).scoped(segment);
    }
    return DomainHandle(materialize(weight, prepared, segment, domain)).scoped(segment);
  }

  if (probe.kind() == FilterCache::Probe::Kind::HIT) {
    return use->effectiveDomain((size_t)segment.ord, reader, domain);
  } else if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
    auto buildStart = std::chrono::steady_clock::now();
    auto raw = materializeRawFilter(weight, prepared, segment);
    uint64_t buildCostNanos = elapsedBuildNanos(buildStart);
    use->publishRawByproduct((size_t)segment.ord, probe, std::move(raw), buildCostNanos);
  } else {
    // A non-folded raw materialization is free by-product population. A
    // domain-composed set must never enter the raw cache.
    if (domain == nullptr && use->wasAdmitted() && !weight.needsScores() && !weight.allowsPruning()) {
      auto buildStart = std::chrono::steady_clock::now();
      auto raw = materializeRawFilter(weight, prepared, segment);
      uint64_t buildCostNanos = elapsedBuildNanos(buildStart);
      use->offerRaw((size_t)segment.ord, std::move(raw), buildCostNanos);
      DocSet* docs = use->effectiveDocSet((size_t)segment.ord, reader);
      return DomainHandle::borrowed(docs).scoped(segment);
    }
    if (!disableFilterOwnForTests && !weight.needsScores() && !weight.allowsPruning()) {
      auto raw = materializeRawFilter(weight, prepared, segment);
      use->adoptOwnedRaw((size_t)segment.ord, probe, std::move(raw));
      return use->effectiveDomain((size_t)segment.ord, reader, domain);
    }
    return DomainHandle(materialize(weight, prepared, segment, domain)).scoped(segment);
  }

  return use->effectiveDomain((size_t)segment.ord, reader, domain);
}

}  // namespace luxir::QueryPrep
