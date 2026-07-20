#pragma once

#include "solux/search/Collector.h"
#include "solux/search/SortField.h"
#include "solux/util/heap.h"
#include "solux/util/solux_util.h"
#include <algorithm>
#include <memory>
#include <vector>

namespace solux {

class FieldSortCollector {
public:
  struct SortDoc {
    segdoc doc;
    float score;
    int32_t slot;

    SortDoc() : doc(), score(0.0f), slot(-1) {}
    SortDoc(segdoc d, float s, int32_t sl) : doc(d), score(s), slot(sl) {}
  };

  enum class CompareSource {
    LOCAL,
    OTHER,
    CURRENT_DOC
  };

  struct FieldSortComparatorFunctor {
    const FieldSortCollector* collector;

    explicit FieldSortComparatorFunctor(const FieldSortCollector* collector)
      : collector(collector) {}

    bool operator()(const SortDoc& docA, const SortDoc& docB) const;
  };

  struct RuntimeClause {
    SortClause descriptor;
    std::unique_ptr<FieldComparator> comparator;

    RuntimeClause(const SortClause& descriptor, int32_t topCount,
                  IndexReader* reader)
      : descriptor(descriptor) {
      if (descriptor.getKind() == SortClause::COLUMN) {
        comparator = descriptor.getSortField().createComparator(topCount, reader);
      }
    }
  };

  int64_t hitCount = 0;
  int64_t topCount;
  std::vector<SortDoc> topDocs;
  std::vector<RuntimeClause> clauses;
  // Single-column plans keep the pre-clause-walk cost on the per-doc admission
  // path: one direct virtual compareBottom instead of the generic clause walk
  // (measured ~40% slower on match-all single-field sort benchmarks).
  FieldComparator* soleColumn = nullptr;
  bool needsSort = true;
  std::unique_ptr<DirectPQ<SortDoc, FieldSortComparatorFunctor>> pq;

public:
  FieldSortCollector(int64_t topCount, const std::vector<SortClause>& clauses,
                     IndexReader* reader = nullptr)
    : topCount(topCount), topDocs(topCount) {
    assert(topCount >= 0);
    assert(!clauses.empty());

    this->clauses.reserve(clauses.size());
    for (const auto& clause : clauses) {
      this->clauses.emplace_back(clause, (int32_t)topCount, reader);
    }
    if (this->clauses.size() == 1 && this->clauses[0].comparator != nullptr) {
      soleColumn = this->clauses[0].comparator.get();
    }

    FieldSortComparatorFunctor comp(this);
    pq = std::make_unique<DirectPQ<SortDoc, FieldSortComparatorFunctor>>(topDocs, comp);
  }

  // The pq's functor holds a back-pointer to this collector; moving would leave
  // it dangling.
  FieldSortCollector(FieldSortCollector&&) = delete;
  FieldSortCollector& operator=(FieldSortCollector&&) = delete;

  void setSegment(int32_t segment, PostingsReader* reader) {
    for (auto& clause : clauses) {
      if (clause.comparator != nullptr) {
        clause.comparator->setSegment(segment, reader);
      }
    }
  }

  // COLUMN comparators bake direction into their stored values (sortMultiplier),
  // so only SCORE/DOC clauses apply direction here.
  int compare(const SortDoc& docA, const SortDoc& docB,
              CompareSource source = CompareSource::LOCAL,
              const FieldSortCollector* other = nullptr) const {
    for (size_t i = 0; i < clauses.size(); i++) {
      const auto& clause = clauses[i];
      int cmp = 0;
      switch (clause.descriptor.getKind()) {
        case SortClause::COLUMN:
          if (source == CompareSource::CURRENT_DOC) {
            cmp = clause.comparator->compareBottom(docA.slot, docA.doc, docB.doc);
          } else if (source == CompareSource::OTHER) {
            assert(other != nullptr);
            cmp = clause.comparator->compare(
              docA.slot, docA.doc, *other->clauses[i].comparator, docB.slot, docB.doc);
          } else {
            cmp = clause.comparator->compare(
              docA.slot, docA.doc, docB.slot, docB.doc);
          }
          break;
        case SortClause::SCORE:
          cmp = (docA.score > docB.score) - (docA.score < docB.score);
          if (clause.descriptor.getOrder() == SortField::DESC) cmp = -cmp;
          break;
        case SortClause::DOC:
          cmp = (docA.doc > docB.doc) - (docA.doc < docB.doc);
          if (clause.descriptor.getOrder() == SortField::DESC) cmp = -cmp;
          break;
      }
      if (cmp != 0) return cmp;
    }
    return (docA.doc > docB.doc) - (docA.doc < docB.doc);
  }

  void copy(int32_t slot, segdoc doc) {
    for (auto& clause : clauses) {
      if (clause.comparator != nullptr) clause.comparator->copy(slot, doc);
    }
  }

  void copy(int32_t slot, FieldSortCollector& other,
            int32_t otherSlot, segdoc otherDoc) {
    for (size_t i = 0; i < clauses.size(); i++) {
      if (clauses[i].comparator != nullptr) {
        clauses[i].comparator->copy(
          slot, *other.clauses[i].comparator, otherSlot, otherDoc);
      }
    }
  }

  // The rejected-doc path must stay small enough to inline into the collectTopK
  // loop (with the clause walk, copy loops, and heap sift inlined here, the
  // per-doc call overhead alone cost ~10% on single-field sort benchmarks), so
  // everything off that path is a SOLUX_NOINLINE helper.
  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;
    if (topCount == 0) return;

    segdoc doc(segment, docid);
    if (pq->size() < (size_t)topCount) {
      warmup(doc, score);
      return;
    }

    SortDoc& bottom = pq->top();
    int cmp;
    if (soleColumn != nullptr) [[likely]] {
      cmp = soleColumn->compareBottom(bottom.slot, bottom.doc, doc);
      if (cmp == 0) {
        cmp = (bottom.doc > doc) - (bottom.doc < doc);
      }
    } else {
      cmp = compareCurrentDoc(bottom, doc, score);
    }
    if (cmp > 0) [[unlikely]] {
      admit(bottom, doc, score);
    }
  }

private:
  SOLUX_NOINLINE void warmup(segdoc doc, float score) {
    int32_t slot = (int32_t)pq->size();
    copy(slot, doc);
    pq->insert(SortDoc(doc, score, slot));
  }

  SOLUX_NOINLINE int compareCurrentDoc(const SortDoc& bottom, segdoc doc, float score) const {
    SortDoc candidate(doc, score, -1);
    return compare(bottom, candidate, CompareSource::CURRENT_DOC);
  }

  SOLUX_NOINLINE void admit(SortDoc& bottom, segdoc doc, float score) {
    copy(bottom.slot, doc);
    bottom = SortDoc(doc, score, bottom.slot);
    pq->updateTop();
  }

public:

  void merge(FieldSortCollector& other) {
    hitCount += other.hitCount;
    needsSort = true;

    for (int64_t i = 0; i < (int64_t)other.pq->size(); i++) {
      const auto& otherDoc = other.topDocs[(size_t)i];
      if (pq->size() < (size_t)topCount) {
        int32_t slot = (int32_t)pq->size();
        copy(slot, other, otherDoc.slot, otherDoc.doc);
        pq->insert(SortDoc(otherDoc.doc, otherDoc.score, slot));
      } else {
        SortDoc& bottom = pq->top();
        if (compare(bottom, otherDoc, CompareSource::OTHER, &other) > 0) {
          copy(bottom.slot, other, otherDoc.slot, otherDoc.doc);
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
    return pq->size();
  }

  std::span<SortDoc> sort() {
    auto finalSize = size();
    if (needsSort) {
      FieldSortComparatorFunctor comp(this);
      std::sort_heap(topDocs.begin(), topDocs.begin() + finalSize, comp);
      needsSort = false;
    }
    return {topDocs.data(), finalSize};
  }

  std::span<SortDoc> scoreDocs() {
    return {topDocs.data(), size()};
  }
};

inline bool FieldSortCollector::FieldSortComparatorFunctor::operator()(
    const SortDoc& docA, const SortDoc& docB) const {
  return collector->compare(docA, docB) < 0;
}

} // namespace solux
