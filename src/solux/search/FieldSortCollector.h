#pragma once

#include "solux/search/Collector.h"
#include "solux/search/FieldComparator.h"
#include "solux/util/heap.h"
#include <vector>
#include <memory>
#include <algorithm>

namespace solux {

class FieldSortCollector {
public:
  struct SortDoc {
    segdoc doc;
    float score;
    int32_t slot;  // Slot index in the comparator
    
    SortDoc() : doc(), score(0.0f), slot(-1) {}
    SortDoc(segdoc d, float s, int32_t sl) : doc(d), score(s), slot(sl) {}
  };

  int64_t hitCount = 0;
  int64_t topCount;
  std::vector<SortDoc> topDocs;
  std::unique_ptr<FieldComparator> comparator;
  int32_t currentSegment = -1;
  bool needsSort = true;  // True if heap needs sorting, false if already sorted

  // Comparator functor that uses the FieldComparator - public for testing
  struct FieldSortComparatorFunctor {
    FieldComparator* comparator;
    
    FieldSortComparatorFunctor(FieldComparator* comp) : comparator(comp) {}

    bool operator()(const SortDoc& docA, const SortDoc& docB) const {
      int cmp = comparator->compare(docA.slot, docA.doc, docB.slot, docB.doc);
      if (cmp != 0) {
        // For a compare for an ascending sort, we want the least competitive (highest sortValue) at the top of the heap
        // This is the normal case for a max-heap, so the sort order is just the natural order.
        // A < B means A is more competitive.
        return cmp < 0;
      }
      
      // Tie-breaker: use segdoc ascending sort
      return docA.doc < docB.doc;
    }
  };

  std::unique_ptr<DirectPQ<SortDoc, FieldSortComparatorFunctor>> pq;

public:
  FieldSortCollector(int64_t topCount, std::unique_ptr<FieldComparator> comp)
    : topCount(topCount),
      topDocs(topCount),
      comparator(std::move(comp)) {
    assert(topCount > 0);
    assert(comparator != nullptr);

    // Initialize all SortDoc objects to ensure no garbage values
    for (int64_t i = 0; i < topCount; i++) {
      topDocs[i] = SortDoc();
    }

    FieldSortComparatorFunctor compFunc(comparator.get());
    pq = std::make_unique<DirectPQ<SortDoc, FieldSortComparatorFunctor>>(topDocs, compFunc);
  }

  ~FieldSortCollector() = default;

  void setSegment(int32_t segment, PostingsReader* reader) {
    currentSegment = segment;
    comparator->setSegment(segment, reader);
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;

    segdoc sdoc(segment, docid);


    if (pq->size() < topCount) {
      // We have space, add to the next slot
      int32_t slot = pq->size();
      comparator->copy(slot, sdoc);
      SortDoc newDoc(sdoc, score, slot);
      topDocs[slot] = newDoc;
      pq->insert(newDoc);

      /*
      // If we just filled the last slot, set bottom to the worst in heap
      if (pq->size() == topCount) {
        comparator->setBottom(pq->top().slot);
      }
      */
    }
    else {
      SortDoc& bottom = pq->top();

      int64_t cmp = comparator->compareBottom(bottom.slot, bottom.doc, sdoc);
      if (cmp == 0) {
        cmp = bottom.doc.compare(sdoc);
      }
      
      if (cmp > 0) {
        comparator->copy(bottom.slot, sdoc);
        bottom = SortDoc(segdoc(segment, docid), score, bottom.slot);
        
        pq->updateTop();

        /*
        // Update bottom to the new worst doc after updateTop
        comparator->setBottom(pq->top().slot);
        */
      }
    }
  }

  void merge(FieldSortCollector& other) {
    // Save the total hit count
    hitCount = hitCount + other.hitCount;
    
    // After merge, the heap will need sorting
    needsSort = true;

    // Iterate through other's documents and conditionally insert into this PQ
    for (int64_t i = 0; i < other.pq->size(); i++) {
      const auto& otherDoc = other.topDocs[i];
      
      if (pq->size() < topCount) {
        // We have space, add to the next slot
        int32_t slot = pq->size();
        comparator->copy(slot, *other.comparator, otherDoc.slot, otherDoc.doc);
        SortDoc newDoc(otherDoc.doc, otherDoc.score, slot);
        topDocs[slot] = newDoc;
        pq->insert(newDoc);
      } else {
        // PQ is full, check if this doc is competitive
        SortDoc& bottom = pq->top();
        
        // Compare other doc with our bottom using cross-comparator compare
        int cmp = comparator->compare(bottom.slot, bottom.doc, *other.comparator, otherDoc.slot, otherDoc.doc);
        if (cmp == 0) {
          // Tie-breaker by segdoc ascending
          cmp = (bottom.doc > otherDoc.doc) - (bottom.doc < otherDoc.doc);
        }

        if (cmp > 0) {
          comparator->copy(bottom.slot, *other.comparator, otherDoc.slot, otherDoc.doc);
          bottom = SortDoc(otherDoc.doc, otherDoc.score, bottom.slot);
          pq->updateTop();
        }
      }
    }
  }

  int64_t totalHits() const {
    return hitCount;
  }

  uint64_t size() const {
    return pq ? pq->size() : 0;
  }

  std::span<SortDoc> sort() {
    auto finalSize = size();
    
    if (needsSort && pq) {
      // Use sort_heap to convert heap to sorted order, just like other collectors
      FieldSortComparatorFunctor comp(comparator.get());
      
      std::sort_heap(topDocs.begin(), topDocs.begin() + finalSize, comp);
    }
    return {topDocs.data(), finalSize};
  }

  std::span<SortDoc> scoreDocs() {
    return {topDocs.data(), size()};
  }
};

} // namespace solux
