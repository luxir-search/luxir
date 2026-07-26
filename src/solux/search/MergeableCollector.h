#pragma once

#include <memory>
#include <vector>

#include "solux/search/Collector.h"
#include "solux/search/FieldSortCollector.h"
#include "solux/search/IndexReader.h"
#include "solux/search/SortField.h"
#include "solux/util/AtomicMerger.h"

namespace solux {

// Merged top-K collector used by TopDocsReq and any op that consumes its
// ranking (e.g. FusionOp).  Holds either a score-sorted collector or a
// field-sorted collector, selected by the sort plan.
// Lives behind an AtomicMerger so multiple per-segment collection tasks
// can hand off their partial results concurrently.
class MergeableCollector : public MergeableData {
public:
  std::unique_ptr<TopDocsCollector> scoreCollector;
  std::unique_ptr<FieldSortCollector> fieldCollector;
  bool useFieldSort;

  MergeableCollector(size_t topCount, const SortPlan& sortPlan,
                     IndexReader* reader, bool needsScores)
    : useFieldSort(sortPlan.useFieldSort) {
    if (!useFieldSort) {
      scoreCollector = std::make_unique<TopDocsCollector>(topCount);
    } else {
      fieldCollector = std::make_unique<FieldSortCollector>(
        topCount, sortPlan.clauses, reader, needsScores);
    }
  }

  /// Number of docs the active arm keeps; 0 for a count-only collection.
  int64_t topCount() const {
    return useFieldSort ? fieldCollector->topCount : scoreCollector->topCount;
  }

  /// Add matches the active arm did not see individually, for callers that
  /// derive part of the count instead of collecting it doc by doc.
  void addHits(int64_t hits) {
    if (useFieldSort) {
      fieldCollector->hitCount += hits;
    } else {
      scoreCollector->hitCount += hits;
    }
  }

  static MergeableCollector* merge(MergeableCollector* a, MergeableCollector* b) {
    // merge the smaller collector into the larger collector, or if both the same size, merge
    // the less competitive collector into the more competitive collector.
    if (a->useFieldSort) {
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
