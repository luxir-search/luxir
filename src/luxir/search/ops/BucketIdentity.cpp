// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/FacetOp.h"
#include "luxir/query/NumericPredicateQuery.h"

namespace luxir {

bool FacetReq::numericIdentitiesLead(size_t segment, const SegFieldInfo& info, DocSet* eligibility,
                                    size_t count,
                                    std::function_ref<PointsReader::ValueRange(size_t)> bounds) {
  if (!eligibility) return true;
  uint64_t remaining = (uint64_t)eligibility->card();
  if (remaining == 0) return false;
  PointsReader points(reader.segments()[segment].postingsReader(), info);
  for (size_t i = 0; i < count; i++) {
    auto range = bounds(i);
    auto fence = points.fenceRange(range.lo, range.hi);
    if (fence.empty) continue;
    uint64_t cost = points.leafOrdinalEnd(fence.lastLeaf) - points.leafOrdinalStart(fence.firstLeaf);
    if (cost >= remaining) return false;
    remaining -= cost;
  }
  // NumericPredicateQuery currently classifies column blocks before receiving
  // demand. A sparse prefix should use one existing joint column pass instead
  // of paying that setup per bucket; broad identities retain its bounded
  // points/bitset materialization. This route never admits identity filters.
  return true;
}

DomainHandle FacetReq::numericPredicateIdentity(size_t segment, int64_t lo, int64_t hi, DocSet* eligibility) {
  auto pool = MemPool::threadLocalPoolGuard();
  NumericPredicateQuery identity(fieldName, lo, hi);
  Query::Context context(pool.pool(), reader);
  auto* weight = identity.createWeight(context, 0);
  return DomainHandle(QueryPrep::materialize(*weight, nullptr, reader.segments()[segment], eligibility,
                                             Query::SupplierExecutionMode::ORDINARY,
                                             QueryPrep::MaterializeMode::DOMAIN_DRIVEN));
}

}  // namespace luxir
