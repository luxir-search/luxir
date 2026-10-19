// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SearchOp.h"
#include "luxir/search/Collapse.h"
#include "luxir/search/Collector.h"
#include "luxir/util/AtomicMerger.h"

namespace luxir {
class TopDocsReq;

struct MetricGroupPlan {
  std::vector<SearchOp*> metrics;
  std::vector<bool> descending;
};

void resolveMetricGroupOrder(TopDocsReq& op);
SearchOp::Calculator* createMetricGroupCalculator(TopDocsReq& op, SearchOp::Calculator* parent,
    int64_t slot, int64_t numSlots, const StructuralConstraint* structural);

// An exhaustive keyed collector, not a top-k document heap. Preparation,
// routing, segment dispatch and page emission belong to TopDocsReq.
class MetricGroupData : public MergeableData {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  struct Rank {
    segdoc doc;
    float score;
    size_t slot;
  };
  MetricGroupData(TopDocsReq& op, SearchOp::Calculator* parent);
  ~MetricGroupData();
  void collect(int32_t segment, Query::Scorer* scorer, DocSet* filter,
               DocSetBuilder* builder, DomainBuildMode mode, DocSet* rawDomain);
  std::span<Rank> sort();
  const CollapseKey& key(size_t slot) const;
  int64_t totalHits() const;
  int64_t totalGroups() const;
  void emitRank(api::ArrVal& out, size_t slot, std::pmr::memory_resource& mr) const;
  static MetricGroupData* merge(MetricGroupData* a, MetricGroupData* b);
};

class MetricGroupState {
public:
  AtomicMerger<MetricGroupData> merger;
  MetricGroupState(TopDocsReq& op, SearchOp::Calculator* parent);
};
}  // namespace luxir
