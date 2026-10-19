// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/search/Collapse.h"
#include "luxir/search/SortField.h"
#include "luxir/query/Query.h"
#include "luxir/search/RequestMemTracker.h"

namespace luxir {

// Page buckets, in rank order. The caller keeps the keys and the finalized
// eligibility domains alive. The source is the SAME prepared source used by
// collection, including the admitted kNN window; it is never prepared again.
class CollapseMembers {
  const CollapsePlan& plan;
  std::span<const CollapseKey> keys;
  RequestMemTracker& tracker;
  RequestMemResource resource;
public:
  CollapseMembers(const CollapsePlan& plan, std::span<const CollapseKey> keys,
                  RequestMemTracker& tracker);
  template <bool Replay, typename Visit>
  void walk(int32_t segment, DocSet* eligibility, Query::SegmentSource* source,
            bool scores, Visit&& visit);
  template <typename Visit>
  void forSegment(int32_t segment, DocSet* eligibility,
                  Query::SegmentSource& source, bool scores, Visit&& visit) {
    walk<true>(segment, eligibility, &source, scores, std::forward<Visit>(visit));
  }
  template <typename Visit>
  void forKeys(int32_t segment, DocSet* eligibility, Visit&& visit) {
    walk<false>(segment, eligibility, nullptr, false, std::forward<Visit>(visit));
  }

};

// Cold response-stage evaluation. Emits the list's actual rank clauses (not
// the head_sort clauses), with their native types and exact missing values.
void emitCollapseRank(luxir::api::ArrVal& out, const SortPlan& sort,
                      IndexReader& reader, segdoc doc, float score,
                      std::pmr::memory_resource& mr);

} // namespace luxir

#include "CollapseMembers.inl"
