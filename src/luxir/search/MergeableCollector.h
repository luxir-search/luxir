// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <vector>

#include "luxir/search/Collector.h"
#include "luxir/search/FieldSortCollector.h"
#include "luxir/search/IndexReader.h"
#include "luxir/search/SortField.h"
#include "luxir/util/AtomicMerger.h"

namespace luxir {

// Merged top-K collector used by TopDocsReq and any op that consumes its
// ranking (e.g. FusionOp).  Holds either a score-sorted collector or a
// field-sorted collector, selected by the sort plan.
// Lives behind an AtomicMerger so multiple per-segment collection tasks
// can hand off their partial results concurrently.
class MergeableCollector : public MergeableData {
public:
  std::unique_ptr<TopDocsCollector> scoreCollector;
  std::unique_ptr<GroupScoreCollector> groupScoreCollector;
  std::unique_ptr<FieldSortCollector> fieldCollector;
  std::unique_ptr<GroupFieldCollector> groupFieldCollector;
  std::unique_ptr<ScoreCollector<true, DiversifyAdmission>> diverseScoreCollector;
  std::unique_ptr<FieldCollector<true, DiversifyAdmission>> diverseFieldCollector;
  bool useFieldSort;

  MergeableCollector(size_t topCount, const SortPlan& sortPlan,
                     IndexReader* reader, bool needsScores, const DiversifyPlan& diversify);

  MergeableCollector(size_t topCount, const SortPlan& sortPlan,
                     IndexReader* reader, bool needsScores, const CollapsePlan* collapse = nullptr)
    : useFieldSort(sortPlan.useFieldSort) {
    if (collapse != nullptr && useFieldSort) {
      groupFieldCollector = std::make_unique<GroupFieldCollector>(
          topCount, sortPlan.clauses, reader, needsScores, collapse);
    } else if (collapse != nullptr) {
      groupScoreCollector = std::make_unique<GroupScoreCollector>(topCount, collapse);
    } else if (!useFieldSort) {
      scoreCollector = std::make_unique<TopDocsCollector>(topCount);
    } else {
      fieldCollector = std::make_unique<FieldSortCollector>(
        topCount, sortPlan.clauses, reader, needsScores);
    }
  }

  template <bool Collapsed, typename Admission = CollapseAdmission>
  auto* scores() {
    if constexpr (Admission::isDiversified) return diverseScoreCollector.get();
    else if constexpr (Collapsed) return groupScoreCollector.get();
    else return scoreCollector.get();
  }

  template <bool Collapsed, typename Admission = CollapseAdmission>
  auto* fields() {
    if constexpr (Admission::isDiversified) return diverseFieldCollector.get();
    else if constexpr (Collapsed) return groupFieldCollector.get();
    else return fieldCollector.get();
  }

  /// Number of docs the active arm keeps; 0 for a count-only collection.
  int64_t topCount() const {
    if (diverseFieldCollector) return diverseFieldCollector->topCount;
    if (diverseScoreCollector) return diverseScoreCollector->topCount;
    if (groupFieldCollector) return groupFieldCollector->topCount;
    if (groupScoreCollector) return groupScoreCollector->topCount;
    return useFieldSort ? fieldCollector->topCount : scoreCollector->topCount;
  }

  /// Add matches the active arm did not see individually, for callers that
  /// derive part of the count instead of collecting it doc by doc.
  void addHits(int64_t hits) {
    if (diverseFieldCollector) {
      diverseFieldCollector->hitCount += hits;
    } else if (diverseScoreCollector) {
      diverseScoreCollector->hitCount += hits;
    } else if (groupFieldCollector) {
      groupFieldCollector->hitCount += hits;
    } else if (groupScoreCollector) {
      groupScoreCollector->hitCount += hits;
    } else if (useFieldSort) {
      fieldCollector->hitCount += hits;
    } else {
      scoreCollector->hitCount += hits;
    }
  }

  static MergeableCollector* mergeDiversified(MergeableCollector* a, MergeableCollector* b);

  static MergeableCollector* merge(MergeableCollector* a, MergeableCollector* b) {
    // merge the smaller collector into the larger collector, or if both the same size, merge
    // the less competitive collector into the more competitive collector.
    if (a->diverseFieldCollector || a->diverseScoreCollector) return mergeDiversified(a, b);
    if (a->groupFieldCollector) {
      if (a->groupFieldCollector->size() < b->groupFieldCollector->size()) std::swap(a, b);
      a->groupFieldCollector->merge(*b->groupFieldCollector);
    } else if (a->groupScoreCollector) {
      if (a->groupScoreCollector->size() < b->groupScoreCollector->size()) std::swap(a, b);
      a->groupScoreCollector->merge(*b->groupScoreCollector);
    } else if (a->useFieldSort) {
      if (a->fieldCollector->size() < b->fieldCollector->size()) {
        std::swap(a, b);
      }
      a->fieldCollector->merge(*b->fieldCollector);
    } else {
      if (a->scoreCollector->size() < b->scoreCollector->size()
        || a->scoreCollector->minCompetitiveVal < b->scoreCollector->minCompetitiveVal) {
        std::swap(a, b);
      }
      a->scoreCollector->merge(*b->scoreCollector);
    }
    return a;
  }
};

}
