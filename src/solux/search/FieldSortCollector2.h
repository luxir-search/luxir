#pragma once

#include "solux/search/Collector.h"
#include "solux/search/FieldComparator.h"
#include "solux/util/heap.h"
#include <vector>
#include <memory>

namespace solux {

class FieldSortCollector2 {
public:
  // Extend ScoreDoc to include the sort value
  SOLUX_PACKED_START
  struct SortDoc {
    float score;
    segdoc doc;
    int64_t sortValue;  // Added field for sorting
  } SOLUX_PACKED_END;

private:
  int64_t hitCount = 0;
  int64_t topCount;
  std::vector<SortDoc> topDocs;
  std::unique_ptr<FieldComparator> comparator;
  int32_t currentSegment = -1;

  // for an ascending compare we want the least competitive (highest sortValue) at the top of the heap
  // This is the normal case for a max-heap, so the sort order is just the natural order
  struct SortComparator {
    bool operator()(const SortDoc& a, const SortDoc& b) const {
      if (a.sortValue != b.sortValue) {
        return a.sortValue < b.sortValue;
      }
      // Tie-breaker
      return a.doc < b.doc;
    }
  };

  DirectPQ<SortDoc, SortComparator> pq;

public:
  FieldSortCollector2(int64_t topCount, std::unique_ptr<FieldComparator> comp)
    : topCount(topCount),
      topDocs(topCount),
      comparator(std::move(comp)),
      pq(topDocs) {
    assert(topCount > 0);
    assert(comparator != nullptr);
  }

  void setSegment(int32_t segment, PostingsReader* reader) {
    currentSegment = segment;
    comparator->setSegment(segment, reader);
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;

    // Get the sort value for this document
    int64_t sortValue = comparator->getDocValue(docid);
    
    SortDoc newDoc{score, segdoc(segment, docid), sortValue};
    
    pq.insertWithOverflow(newDoc);
  }

  void merge(FieldSortCollector2& other) {
    // Save the total hit count
    hitCount = hitCount + other.hitCount;
    
    // Merge by re-collecting all docs from other
    // This maintains the heap invariant properly
    auto newHitCount = hitCount;
    for (int i = other.size() - 1; i >= 0; i--) {
      SortDoc& sd = other.topDocs[i];
      // We already have the sortValue, so just insert directly
      pq.insertWithOverflow(sd);
    }
    hitCount = newHitCount;
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return pq.size();
  }

  // Sort the results in proper order (best first)
  std::span<SortDoc> sort() {
    // Just like TopDocsCollector, use sort_heap with our comparator
    SortComparator comp;
    std::sort_heap(topDocs.begin(), topDocs.begin() + pq.size(), comp);
    return {topDocs.data(), pq.size()};
  }

  std::span<SortDoc> scoreDocs() {
    return {topDocs.data(), pq.size()};
  }
};

} // namespace solux