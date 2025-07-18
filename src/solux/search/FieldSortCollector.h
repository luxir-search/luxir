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
    };
    
private:
    int64_t hitCount = 0;
    int64_t topCount;
    std::vector<SortDoc> topDocs;
    std::unique_ptr<FieldComparator> comparator;
    int32_t currentSegment = -1;
    
    struct FieldSortComparator {
        FieldComparator* comp;
        
        FieldSortComparator(FieldComparator* c) : comp(c) {}
        
        bool operator()(const SortDoc& docA, const SortDoc& docB) const {
            int cmp = comp->compare(docA.doc.docId(), docA.doc.segment(), 
                                   docB.doc.docId(), docB.doc.segment());
            if (cmp != 0) {
                // For a priority queue, returning true means docA has lower priority than docB
                // We want the opposite of the sort order at the top of the heap
                return cmp < 0;  // This creates a min-heap based on the comparator
            }
            
            if (docA.score != docB.score) {
                return docA.score < docB.score;  // Lower scores have lower priority
            }
            
            return docA.doc > docB.doc;  // Higher docids have lower priority
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
        
        FieldSortComparator compFunc(comparator.get());
        pq = std::make_unique<DirectPQ<SortDoc, FieldSortComparator>>(topDocs, compFunc);
    }
    
    ~FieldSortCollector() = default;
    
    void setSegment(int32_t segment, PostingsReader* reader) {
        currentSegment = segment;
        comparator->setSegment(segment, reader);
    }
    
    void collect(int32_t segment, int32_t docid, float score) {
        hitCount++;
        
        
        if (currentSegment != segment) {
            currentSegment = segment;
        }
        
        SortDoc newDoc{segdoc(segment, docid), score};
        
        if (pq->size() < topCount) {
            int32_t slot = pq->size();
            topDocs[slot] = newDoc;
            comparator->copy(slot, docid, segment);
            pq->insert(newDoc);
            comparator->setBottom(pq->size() - 1);
        } else {
            // For heap, we need to check against the top (worst) element
            comparator->setBottom(0); // top of min-heap
            if (comparator->compareBottom(docid, segment) > 0) {
                // Better than worst element, so replace it
                topDocs[0] = newDoc; // Overwrite top
                comparator->copy(0, docid, segment);
                pq->updateTop();
            }
        }
    }
    
    void merge(FieldSortCollector& other) {
        auto newHitCount = hitCount + other.hitCount;
        
        for (int64_t i = 0; i < other.size(); i++) {
            auto& sortDoc = other.topDocs[i];
            collect(sortDoc.doc.segment(), sortDoc.doc.docId(), sortDoc.score);
        }
        
        hitCount = newHitCount;
    }
    
    int64_t totalHits() const {
        return hitCount;
    }
    
    int64_t size() const {
        return pq->size();
    }
    
    std::span<SortDoc> sort() {
        int64_t numDocs = pq->size();
        std::vector<SortDoc> sorted;
        sorted.reserve(numDocs);
        
        while (pq->size() > 0) {
            sorted.push_back(pq->removeTop());
        }
        
        std::reverse(sorted.begin(), sorted.end());
        
        
        std::copy(sorted.begin(), sorted.end(), topDocs.begin());
        
        return {topDocs.data(), numDocs};
    }
    
    std::span<SortDoc> scoreDocs() {
        return {topDocs.data(), pq->size()};
    }
};

} // namespace solux