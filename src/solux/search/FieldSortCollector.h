#pragma once

#include "solux/search/Collector.h"
#include "solux/search/FieldComparator.h"
#include "solux/util/heap.h"
#include <vector>
#include <memory>

namespace solux {

class FieldSortCollector {
public:
  struct SortDoc {
    segdoc doc;
    float score;
    int64_t sortValue;  // The actual field value used for sorting
    
    SortDoc() : doc(), score(0.0f), sortValue(0) {}
    SortDoc(segdoc d, float s, int64_t v) : doc(d), score(s), sortValue(v) {}
  };

private:
  int64_t hitCount = 0;
  int64_t topCount;
  std::vector<SortDoc> topDocs;
  std::unique_ptr<FieldComparator> comparator;
  int32_t currentSegment = -1;

  struct FieldSortComparator {
    FieldSortComparator() {}

    bool operator()(const SortDoc& docA, const SortDoc& docB) const {
      // For top-K collection, we need a max-heap regardless of sort order
      // This way, the "worst" of the K best is at the top and can be replaced
      // The values are already adjusted for sort order (negated if descending)
      if (docA.sortValue != docB.sortValue) {
        return docA.sortValue > docB.sortValue; // MAX heap
      }

      // For tie-breaking: higher segments/docs should be at top (easier to replace)
      return docA.doc > docB.doc;
    }
  };

  std::unique_ptr<DirectPQ<SortDoc, FieldSortComparator>> pq;

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

    FieldSortComparator compFunc;
    pq = std::make_unique<DirectPQ<SortDoc, FieldSortComparator>>(topDocs, compFunc);
  }

  ~FieldSortCollector() = default;

  void setSegment(int32_t segment, PostingsReader* reader) {
    currentSegment = segment;
    comparator->setSegment(segment, reader);
    // Debug: print segment processing order
    // std::cout << "FieldSortCollector: Processing segment " << segment << "\n";
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;

    if (currentSegment != segment) {
      currentSegment = segment;
    }

    // Get the sort value directly
    int64_t sortValue = comparator->getDocValue(docid);
    SortDoc newDoc(segdoc(segment, docid), score, sortValue);
    
    // Debug output
    // std::cout << "Collecting doc " << docid << " seg " << segment << " sortValue=" << sortValue << "\n";

    if (pq->size() < topCount) {
      int32_t slot = pq->size();
      topDocs[slot] = newDoc;
      pq->insert(newDoc);
    }
    else {
      // Use the heap's insertWithOverflow which handles the comparison correctly
      pq->insertWithOverflow(newDoc);
    }
  }

  void merge(FieldSortCollector& other) {
    // Save the total hit count
    hitCount = hitCount + other.hitCount;
    
    // If the other collector is empty, nothing to merge
    if (other.size() == 0) {
      return;
    }
    
    // Since we can't use the field comparator across segments,
    // we need to collect all documents and re-sort them.
    // For now, we'll sort by score and then by segdoc for determinism.
    
    // Get all documents from both collectors without modifying them
    std::vector<SortDoc> allDocs;
    allDocs.reserve(this->size() + other.size());
    
    // Add our documents
    for (int64_t i = 0; i < this->size(); i++) {
      allDocs.push_back(topDocs[i]);
    }
    
    // Add other's documents
    for (int64_t i = 0; i < other.size(); i++) {
      allDocs.push_back(other.topDocs[i]);
    }
    
    // Sort all documents by their field values and then by segdoc for determinism
    // The sortValue already has the reversed flag applied (negated if descending)
    // so we always sort in ascending order of sortValue
    std::sort(allDocs.begin(), allDocs.end(), [](const SortDoc& a, const SortDoc& b) {
      if (a.sortValue != b.sortValue) {
        return a.sortValue < b.sortValue; // Always ascending by stored value
      }
      // Use segdoc as tiebreaker for determinism
      // segdoc comparison: compares segment first (high bits), then docid (low bits)
      return a.doc < b.doc; // Lower segdoc first
    });
    
    // Keep only top K
    size_t keepCount = std::min(topCount, (int64_t)allDocs.size());
    
    // Copy the top K documents to our array
    for (size_t i = 0; i < keepCount; i++) {
      topDocs[i] = allDocs[i];
    }
    
    // Rebuild the heap with the exact size and documents
    // Use the DirectPQ constructor that takes currentSize to build heap correctly
    FieldSortComparator compFunc;
    pq = std::make_unique<DirectPQ<SortDoc, FieldSortComparator>>(std::span<SortDoc>(topDocs.data(), topCount), compFunc, keepCount);
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return pq->size();
  }

  std::span<SortDoc> sort() {
    int64_t numDocs = pq->size();
    
    // Debug: print what's in the heap before sorting
    // std::cout << "Before sort, heap size=" << numDocs << "\n";
    // for (int i = 0; i < numDocs; i++) {
    //   std::cout << "  topDocs[" << i << "]: seg=" << topDocs[i].doc.segment() 
    //             << " doc=" << topDocs[i].doc.docid() 
    //             << " sortValue=" << topDocs[i].sortValue << "\n";
    // }
    
    std::vector<SortDoc> sorted;
    sorted.reserve(numDocs);

    while (pq->size() > 0) {
      sorted.push_back(pq->removeTop());
    }

    // We have a max-heap, so removeTop gives us largest sortValue to smallest sortValue
    // But we want smallest sortValue first (best results first)
    // So we need to reverse
    std::reverse(sorted.begin(), sorted.end());

    std::copy(sorted.begin(), sorted.end(), topDocs.begin());

    return {topDocs.data(), numDocs};
  }

  std::span<SortDoc> scoreDocs() {
    return {topDocs.data(), pq->size()};
  }
};

} // namespace solux
