#pragma once

#include "solux/search/Collector.h"
#include "solux/search/SortField.h"
#include "solux/util/heap.h"
#include "solux/util/solux_util.h"
#include "solux/value/ValueExpr.h"
#include <algorithm>
#include <bit>
#include <memory>
#include <memory_resource>
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
    struct ExprSlot {
      union {
        int64_t intValue;
        double doubleValue;
      } value{};
      bool valid = false;
    };
    std::vector<ExprSlot> exprSlots;
    BoundValueProgram* expression = nullptr;
    mutable ValueResult cachedResult;
    mutable segdoc cachedDoc;
    mutable uint32_t cachedScoreBits = 0;
    mutable bool cacheValid = false;

    RuntimeClause(const SortClause& descriptor, int32_t topCount,
                  IndexReader* reader)
      : descriptor(descriptor) {
      if (descriptor.getKind() == SortClause::COLUMN) {
        comparator = descriptor.getSortField().createComparator(topCount, reader);
      } else if (descriptor.getKind() == SortClause::EXPR) {
        exprSlots.resize((size_t)topCount);
      }
    }

    bool isDoubleExpr() const {
      return descriptor.getValueProgram().root().type == ValueType::DOUBLE;
    }

    void resetCache() { cacheValid = false; }

    const ValueResult& current(segdoc doc, float score) const {
      assert(expression != nullptr);
      uint32_t scoreBits = std::bit_cast<uint32_t>(score);
      if (!cacheValid || cachedDoc != doc || cachedScoreBits != scoreBits) {
        cachedResult = expression->evalPoint(doc.docId(), score);
        cachedDoc = doc;
        cachedScoreBits = scoreBits;
        cacheValid = true;
      }
      return cachedResult;
    }

    void copyCurrent(int32_t slot, segdoc doc, float score) {
      const ValueResult& result = current(doc, score);
      ExprSlot& target = exprSlots[(size_t)slot];
      target.valid = result.valid;
      if (!result.valid) return;
      if (isDoubleExpr()) target.value.doubleValue = result.doubleValue;
      else target.value.intValue = result.intValue;
    }

    void copyFrom(int32_t slot, const RuntimeClause& other, int32_t otherSlot) {
      exprSlots[(size_t)slot] = other.exprSlots[(size_t)otherSlot];
    }

    int compareValues(const ExprSlot& left, const ExprSlot& right) const {
      if (left.valid != right.valid) return left.valid ? -1 : 1;
      if (!left.valid) return 0;
      int cmp = isDoubleExpr()
          ? (left.value.doubleValue > right.value.doubleValue)
              - (left.value.doubleValue < right.value.doubleValue)
          : (left.value.intValue > right.value.intValue)
              - (left.value.intValue < right.value.intValue);
      if (descriptor.getOrder() == SortField::DESC) cmp = -cmp;
      return cmp;
    }

    int compareSlots(int32_t left, const RuntimeClause& other, int32_t right) const {
      return compareValues(exprSlots[(size_t)left], other.exprSlots[(size_t)right]);
    }

    int compareCurrent(int32_t left, segdoc doc, float score) const {
      const ValueResult& result = current(doc, score);
      ExprSlot candidate;
      candidate.valid = result.valid;
      if (result.valid) {
        if (isDoubleExpr()) candidate.value.doubleValue = result.doubleValue;
        else candidate.value.intValue = result.intValue;
      }
      return compareValues(exprSlots[(size_t)left], candidate);
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
  RuntimeClause* soleExpr = nullptr;
  bool hasExpr = false;
  bool needsSort = true;
  std::unique_ptr<DirectPQ<SortDoc, FieldSortComparatorFunctor>> pq;

  class ExpressionBindings {
  public:
    MemPool::ScopeGuard scope;
    FieldSortCollector& collector;
    std::pmr::vector<u_ptr<BoundValueProgram>> owned;

    ExpressionBindings(FieldSortCollector& collector, MemPool& pool,
                       PostingsReader& postings)
        : scope(pool), collector(collector), owned(&pool) {
      try {
        for (RuntimeClause& clause : collector.clauses) {
          if (clause.descriptor.getKind() != SortClause::EXPR) continue;
          owned.push_back(clause.descriptor.getValueProgram().bind(pool, postings));
          clause.expression = owned.back().get();
          clause.resetCache();
        }
      } catch (...) {
        clearPointers();
        throw;
      }
    }

    ~ExpressionBindings() {
      clearPointers();
      owned.clear();
    }

    ExpressionBindings(const ExpressionBindings&) = delete;
    ExpressionBindings& operator=(const ExpressionBindings&) = delete;

  private:
    void clearPointers() {
      for (RuntimeClause& clause : collector.clauses) {
        if (clause.descriptor.getKind() == SortClause::EXPR) {
          clause.expression = nullptr;
          clause.resetCache();
        }
      }
    }
  };

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
    } else if (this->clauses.size() == 1
               && this->clauses[0].descriptor.getKind() == SortClause::EXPR) {
      soleExpr = &this->clauses[0];
    }
    for (const RuntimeClause& clause : this->clauses) {
      hasExpr |= clause.descriptor.getKind() == SortClause::EXPR;
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
      clause.resetCache();
      if (clause.comparator != nullptr) {
        clause.comparator->setSegment(segment, reader);
      }
    }
    if (topCount > 0 && pq->size() == (size_t)topCount) setBottom();
  }

  // COLUMN comparators bake direction into their stored values (sortMultiplier).
  // SCORE, DOC, and EXPR apply direction in this clause walk.
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
        case SortClause::EXPR:
          if (source == CompareSource::CURRENT_DOC) {
            cmp = clause.compareCurrent(docA.slot, docB.doc, docB.score);
          } else if (source == CompareSource::OTHER) {
            assert(other != nullptr);
            cmp = clause.compareSlots(docA.slot, other->clauses[i], docB.slot);
          } else {
            cmp = clause.compareSlots(docA.slot, clause, docB.slot);
          }
          break;
      }
      if (cmp != 0) return cmp;
    }
    return (docA.doc > docB.doc) - (docA.doc < docB.doc);
  }

  void copy(int32_t slot, segdoc doc, float score) {
    for (auto& clause : clauses) {
      if (clause.comparator != nullptr) clause.comparator->copy(slot, doc);
      else if (clause.descriptor.getKind() == SortClause::EXPR) {
        clause.copyCurrent(slot, doc, score);
      }
    }
  }

  void copy(int32_t slot, FieldSortCollector& other,
            int32_t otherSlot, segdoc otherDoc) {
    for (size_t i = 0; i < clauses.size(); i++) {
      if (clauses[i].comparator != nullptr) {
        clauses[i].comparator->copy(
          slot, *other.clauses[i].comparator, otherSlot, otherDoc);
      } else if (clauses[i].descriptor.getKind() == SortClause::EXPR) {
        clauses[i].copyFrom(slot, other.clauses[i], otherSlot);
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
    } else if (soleExpr != nullptr) {
      cmp = soleExpr->compareCurrent(bottom.slot, doc, score);
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
    copy(slot, doc, score);
    pq->insert(SortDoc(doc, score, slot));
    if (pq->size() == (size_t)topCount) setBottom();
  }

  SOLUX_NOINLINE int compareCurrentDoc(const SortDoc& bottom, segdoc doc, float score) const {
    SortDoc candidate(doc, score, -1);
    return compare(bottom, candidate, CompareSource::CURRENT_DOC);
  }

  SOLUX_NOINLINE void admit(SortDoc& bottom, segdoc doc, float score) {
    copy(bottom.slot, doc, score);
    bottom = SortDoc(doc, score, bottom.slot);
    pq->updateTop();
    setBottom();
  }

  void setBottom() {
    int32_t slot = pq->top().slot;
    for (auto& clause : clauses) {
      if (clause.comparator != nullptr) clause.comparator->setBottom(slot);
    }
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
