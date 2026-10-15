// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/query/PostingsUnion.h"
#include "luxir/search/Collector.h"
#include "luxir/search/SortField.h"
#include "luxir/util/heap.h"
#include "luxir/util/luxir_util.h"
#include "luxir/value/ValueExpr.h"
#include <algorithm>
#include <bit>
#include <memory>
#include <memory_resource>
#include <vector>

namespace luxir {

class FieldSortCollector {
  static constexpr size_t KEY_BUFFER_SIZE = 1024;
  int64_t keys[KEY_BUFFER_SIZE];

public:
  inline static bool disableKeyGatherForTests = false;
  // Overrides the priced leaf-descent decision (descendsToLeaves).
  inline static std::optional<bool> forceLeafDescentForTests;

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
    // EXPR-only state lives behind one pointer so COLUMN/SCORE/DOC clauses
    // stay compact in the clauses array the per-doc walk strides over.
    struct ExprState {
      std::vector<ExprSlot> slots;
      BoundValueProgram* expression = nullptr;
      ValueResult cachedResult;
      segdoc cachedDoc;
      uint32_t cachedScoreBits = 0;
      bool cacheValid = false;
      bool isDouble = false;
      bool desc = false;
      bool missingFirst = false;
      // A missing literal makes every slot valid: missing values take it.
      bool hasLiteral = false;
      ExprSlot literal;
    };
    std::unique_ptr<ExprState> expr;

    RuntimeClause(const SortClause& descriptor, IndexReader* reader)
      : descriptor(descriptor) {
      if (descriptor.getKind() == SortClause::COLUMN) {
        comparator = descriptor.getSortField().createComparator(reader);
      } else if (descriptor.getKind() == SortClause::EXPR) {
        expr = std::make_unique<ExprState>();
        expr->isDouble =
            descriptor.getValueProgram().root().type == ValueType::DOUBLE;
        expr->desc = descriptor.getOrder() == SortField::DESC;
        const SortClause::ExprMissing& missing = descriptor.getExprMissing();
        expr->missingFirst = missing.edge == FieldComparator::MISSING_FIRST;
        expr->hasLiteral = missing.hasLiteral;
        if (missing.hasLiteral) {
          expr->literal.valid = true;
          if (expr->isDouble) expr->literal.value.doubleValue = missing.doubleValue;
          else expr->literal.value.intValue = missing.intValue;
        }
      }
    }

    ExprSlot slotOf(const ValueResult& result) const {
      if (!result.valid && expr->hasLiteral) return expr->literal;
      ExprSlot slot;
      slot.valid = result.valid;
      if (result.valid) {
        if (expr->isDouble) slot.value.doubleValue = result.doubleValue;
        else slot.value.intValue = result.intValue;
      }
      return slot;
    }

    void resetCache() {
      if (expr != nullptr) expr->cacheValid = false;
    }

    const ValueResult& current(segdoc doc, float score) const {
      ExprState& state = *expr;
      assert(state.expression != nullptr);
      uint32_t scoreBits = std::bit_cast<uint32_t>(score);
      if (!state.cacheValid || state.cachedDoc != doc
          || state.cachedScoreBits != scoreBits) {
        state.cachedResult = state.expression->evalPoint(doc.docId(), score);
        state.cachedDoc = doc;
        state.cachedScoreBits = scoreBits;
        state.cacheValid = true;
      }
      return state.cachedResult;
    }

    LUXIR_NOINLINE void copyCurrent(int32_t slot, segdoc doc, float score) {
      expr->slots[(size_t)slot] = slotOf(current(doc, score));
    }

    void copyFrom(int32_t slot, const RuntimeClause& other, int32_t otherSlot) {
      expr->slots[(size_t)slot] = other.expr->slots[(size_t)otherSlot];
    }

    int compareValues(const ExprSlot& left, const ExprSlot& right) const {
      // Missing values sit at a fixed edge under either direction.
      if (left.valid != right.valid) {
        return (left.valid ? -1 : 1) * (expr->missingFirst ? -1 : 1);
      }
      if (!left.valid) return 0;
      int cmp = expr->isDouble
          ? (left.value.doubleValue > right.value.doubleValue)
              - (left.value.doubleValue < right.value.doubleValue)
          : (left.value.intValue > right.value.intValue)
              - (left.value.intValue < right.value.intValue);
      if (expr->desc) cmp = -cmp;
      return cmp;
    }

    LUXIR_NOINLINE int compareSlots(int32_t left, const RuntimeClause& other, int32_t right) const {
      return compareValues(expr->slots[(size_t)left], other.expr->slots[(size_t)right]);
    }

    LUXIR_NOINLINE int compareCurrent(int32_t left, segdoc doc, float score) const {
      return compareValues(expr->slots[(size_t)left], slotOf(current(doc, score)));
    }
  };

  int64_t hitCount = 0;
  int64_t topCount;
  // Slot storage (comparator values, EXPR slots) is grown on demand rather
  // than allocated for topCount up front.  Slots are minted densely in
  // increasing order, and only on cold paths (warmup, collectWindow's fill
  // loop, an underfull merge), so those paths grow all clauses in lockstep
  // before the first write to a new slot.  slotCapacity is what has been
  // grown so far; it doubles up to topCount.
  int64_t slotCapacity = 0;
  std::vector<RuntimeClause> clauses;
  // Single-column plans keep the pre-clause-walk cost on the per-doc admission
  // path: one direct virtual compareBottom instead of the generic clause walk
  // (measured ~40% slower on match-all single-field sort benchmarks).
  FieldComparator* soleColumn = nullptr;
  RuntimeClause* soleExpr = nullptr;
  bool hasExpr = false;
  bool needsScores = true;
  bool needsSort = true;
  // Owns the SortDoc storage and expands it as hits arrive; topCount bounds
  // the heap but is not allocated up front (limit=-1 maps to maxDoc).
  ExpandingPQ<SortDoc, FieldSortComparatorFunctor> pq;

  // String-sort candidate pruning state, all segment-local. The union is a
  // conservative candidate superset built from the primary comparator's
  // competitive ord interval; it, the activation gate's state, and every
  // allocation behind them live in segmentPool, which the dispatching op
  // owns for the segment's lifetime.
  static constexpr int32_t CANDIDATE_PROBE_INTERVAL = 4096;
  static constexpr int64_t CANDIDATE_STATE_BYTES = 8 << 20;
  // The union's costs in walked source docs: per captured term (a dictionary
  // seek, a stats decode, a postings-state capture, and heap traffic after
  // that) and per posting it reads (a heap cycle on sparse terms, a candidate
  // consult on dense ones). Both sit above what they measure (about 1.5x
  // and 2x), where activations fell closest to the measured best; an active
  // union is never abandoned.
  static constexpr int64_t CANDIDATE_TERM_CAPTURE_COST = 20;
  static constexpr int64_t CANDIDATE_POSTING_COST = 8;
  MemPool* segmentPool = nullptr;
  UnionHeapScorer* candidates = nullptr;
  int64_t candidateLo = 0;
  int64_t candidateHi = -1;
  // The activation gate's docFreq totals, walked outward from the competitive
  // interval's anchored end (CompetitiveOrdState): candidateDocFreqs[j] sums
  // the docFreqs of the j + 1 ords nearest it, the first candidateReach of
  // them filled. The interval only shrinks toward its anchor within a
  // segment, so every later total is an entry here or, past the reach, at
  // least the last one: no docFreq is read twice in a segment. The gate and
  // the capture walk share candidateTerms; the comparator's own enum backs
  // setBottom/copy caching and must not be repositioned here.
  TermsEnum* candidateTerms = nullptr;
  std::span<int64_t> candidateDocFreqs;
  int64_t candidateReach = 0;
  int64_t segmentSourceCost = 0;
  // Source docs of the segments collected before this one: the heap's
  // bottom already ranks k among them.
  int64_t priorSourceDocs = 0;
  // The docs a walk over the segment expects to collect (setWalkCard); 0
  // until a walk names it, when the source cost stands in.
  int64_t segmentWalkCard = 0;
  int32_t segmentMaxDoc = 0;
  // The values every doc this collector will see holds in the primary sort
  // column (a request fact, from the query and filters; see
  // TopDocsReq::sortValueEnvelope). Per segment it shapes primaryKeys: the
  // domain's keys follow the column's in-envelope values, and none lies
  // before the envelope's best edge (SegmentKeys::keyFloor), so no
  // bound-driven traversal can prove out a leaf bounded before that edge
  // (leavesBeforeKeyFloor). Either only ever widens what is collected; an
  // envelope can cost time but never results.
  std::optional<Query::ValueEnvelope> primaryValues;
  // The current segment's primary keys, when its column offers leaf
  // bounds.
  std::optional<FieldComparator::SegmentKeys> primaryKeys;
  int64_t leavesBeforeKeyFloor = 0;
  // Leaves holding no value inside primaryValues (SegmentKeys::leavesOutside).
  FieldComparator::SegmentKeys::Outside leavesOutsideValues;
  // Whether nextCompetitiveRange descends to leaves (descendsToLeaves),
  // decided at its first full-heap consult of a segment.
  enum class LeafDescent : uint8_t { UNDECIDED, ON, OFF };
  LeafDescent leafDescent = LeafDescent::UNDECIDED;
  int32_t nextProbeDoc = 0;
  bool candidatesDisabled = false;

  class ExpressionBindings {
  public:
    MemPool::ScopeGuard scope;
    FieldSortCollector& collector;

    ExpressionBindings(FieldSortCollector& collector, MemPool& pool,
                       IndexReader::Segment& segment)
        : scope(pool), collector(collector) {
      try {
        for (RuntimeClause& clause : collector.clauses) {
          if (clause.descriptor.getKind() != SortClause::EXPR) continue;
          clause.expr->expression =
              clause.descriptor.getValueProgram().bind(pool, segment);
          clause.resetCache();
        }
      } catch (...) {
        clearPointers();
        throw;
      }
    }

    ~ExpressionBindings() {
      clearPointers();
    }

    ExpressionBindings(const ExpressionBindings&) = delete;
    ExpressionBindings& operator=(const ExpressionBindings&) = delete;

  private:
    void clearPointers() {
      for (RuntimeClause& clause : collector.clauses) {
        if (clause.descriptor.getKind() == SortClause::EXPR) {
          clause.expr->expression = nullptr;
          clause.resetCache();
        }
      }
    }
  };

public:
  FieldSortCollector(int64_t topCount, const std::vector<SortClause>& clauses,
                     IndexReader* reader = nullptr, bool needsScores = true)
    : topCount(topCount), needsScores(needsScores),
      pq((size_t)std::max(topCount, (int64_t)0), FieldSortComparatorFunctor(this)) {
    assert(topCount >= 0);
    // Slot indexes are int32 (SortDoc::slot, FieldComparator::growSlots), so
    // the capacity math in growSlotStorage never narrows out of range.
    assert(topCount <= std::numeric_limits<int32_t>::max());
    assert(!clauses.empty());

    this->clauses.reserve(clauses.size());
    for (const auto& clause : clauses) {
      this->clauses.emplace_back(clause, reader);
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
  }

  // The pq's functor holds a back-pointer to this collector; moving would leave
  // it dangling.
  FieldSortCollector(FieldSortCollector&&) = delete;
  FieldSortCollector& operator=(FieldSortCollector&&) = delete;

  // sourceCost: estimated docs the collection source will still produce for
  // this segment (query cost capped by any filter cardinality). It is the
  // baseline candidate pruning must beat; negative means unknown, which
  // conservatively disables candidate activation.
  void setSegment(int32_t segment, PostingsReader* reader,
                  MemPool* pool = nullptr, int64_t sourceCost = -1) {
    segmentPool = pool;
    candidates = nullptr;
    candidateLo = 0;
    candidateHi = -1;
    candidateTerms = nullptr;
    candidateDocFreqs = {};
    candidateReach = 0;
    nextProbeDoc = 0;
    candidatesDisabled = false;
    if (segmentSourceCost > 0) {
      priorSourceDocs += std::min<int64_t>(segmentSourceCost, segmentMaxDoc);
    }
    segmentSourceCost = sourceCost;
    segmentWalkCard = 0;
    segmentMaxDoc = reader != nullptr ? reader->maxDoc() : 0;
    for (auto& clause : clauses) {
      clause.resetCache();
      if (clause.comparator != nullptr) {
        clause.comparator->setSegment(segment, reader);
      }
    }
    leafDescent = LeafDescent::UNDECIDED;
    leavesBeforeKeyFloor = 0;
    primaryKeys.reset();
    FieldComparator::KeyBatch* batch;
    if (topCount > 0 && clauses[0].comparator != nullptr
        && (batch = clauses[0].comparator->keyBatch()) != nullptr
        && batch->leafBlockSize() != 0) {
      primaryKeys = batch->segmentKeys(primaryValues);
    }
    leavesOutsideValues = {};
    if (primaryKeys.has_value() && primaryValues.has_value()) {
      leavesBeforeKeyFloor =
          primaryKeys->leavesBoundedBefore(primaryKeys->keyFloor());
      leavesOutsideValues = primaryKeys->leavesOutside();
    }
    if (topCount > 0 && pq.size() == (size_t)topCount) setBottom();
  }

  // The docs a walk over the current segment expects to collect, which
  // prices its leaf descent (descendsToLeaves). The source cost overstates
  // them for a conjunction (its lead's cost), and the domain decides both
  // how far the bottom matures and how many docs a skipped leaf holds.
  void setWalkCard(int64_t card) { segmentWalkCard = card; }

  // The predicted heap bottom once a segment's domain - `card` docs whose
  // keys follow `segment`'s in-envelope values - is collected
  // into a heap of `heapSize` of topCount entries (bottomKey: the bottom of
  // a full one): the topCount-th best key of the heap's and the domain's,
  // estimated from the value sketch with the domain independent of the key
  // within the envelope. nullopt when the heap does not fill. The heap's
  // entries are read as following the segment's distribution: a heap that
  // is not full holds heapSize more docs drawn from it, and a full one with
  // e domain docs before its bottom (fraction f_b of the domain) keeps the
  // entries the merge leaves it as its own f_b-quantile does, which puts
  // the new bottom at fraction f_b * k / (k + e). Both are exact for
  // segments drawn alike, and a domain wholly past a full bottom leaves it.
  // This is the one prior every field-sort decision prices: the leaves
  // bounded at or before the predicted bottom are the visit floor
  // (predictedVisitFloor), whatever the leaf geometry - wide zones give the
  // density floor k * maxDoc / card, tight (doc-order correlated) zones
  // about k / m leaves in either direction, and a range on the sort field
  // itself puts the bottom inside it.
  static std::optional<int64_t> predictBottom(
      const FieldComparator::SegmentKeys& segment, int64_t card,
      int64_t topCount, int64_t heapSize, int64_t bottomKey) {
    assert(topCount > 0 && card > 0 && segment.values() > 0);
    double k = (double)topCount;
    if (heapSize < topCount) {
      double fraction = k / ((double)heapSize + (double)card);
      if (fraction >= 1) return std::nullopt;
      return segment.keyAt(fraction);
    }
    double before = segment.fractionBefore(bottomKey);
    if (before <= 0) return bottomKey;
    double entering = (double)card * before;
    return std::min(bottomKey, segment.keyAt(before * k / (k + entering)));
  }

  // The leaves a bound-driven traversal visits when the bottom settles at
  // `bottom` over a domain of `card` docs: those bounded strictly before it,
  // and of those bounded exactly at it only enough to hold the domain docs
  // still needed at that key - the tie rule skips a leaf whose best key ties
  // the bottom once the heap's tied entries come first in segdoc order, so
  // a heavily tied bottom needs its first few tied leaves, not all of them.
  static int64_t visitFloorAt(const FieldComparator::SegmentKeys& segment,
                              int64_t bottom, int64_t card, int64_t topCount) {
    int64_t strict = segment.leavesBoundedBefore(bottom);
    int64_t tied = segment.leavesBoundedAtOrBefore(bottom) - strict;
    if (tied <= 1) return strict + tied;
    double before = segment.fractionBefore(bottom);
    double through = bottom == std::numeric_limits<int64_t>::max()
        ? 1.0 : segment.fractionBefore(bottom + 1);
    double needed = std::max(1.0, (double)topCount - (double)card * before);
    double perLeaf = (double)card * (through - before) / (double)tied;
    if (perLeaf <= 0) return strict + tied;
    // Clamped before the cast: a vanishing perLeaf sends the quotient past
    // int64's range.
    return strict
        + (int64_t)std::clamp(std::ceil(needed / perLeaf), 1.0, (double)tied);
  }

  // Leaves of the current segment bounded at or before the predicted
  // bottom (predictBottom) once its `card` domain docs are collected into
  // the heap as it stands: the leaves a bound-driven traversal is expected
  // to visit. Every leaf when the heap does not fill; none for an empty
  // domain. A `depth` past topCount predicts that deeper key instead, the
  // heap read as holding the same share of it.
  int64_t predictedVisitFloor(int64_t card, int64_t depth = 0) const {
    assert(primaryKeys.has_value());
    if (card <= 0 || primaryKeys->values() <= 0) return 0;
    if (depth <= 0) depth = topCount;
    int64_t bottomKey = heapFull() ? bottomPrimaryKey() : 0;
    int64_t heapSize = (int64_t)pq.size() * depth / topCount;
    auto bottom = predictBottom(*primaryKeys, card, depth, heapSize,
                                bottomKey);
    return bottom.has_value()
        ? visitFloorAt(*primaryKeys, *bottom, card, depth)
        : primaryKeys->leafCount();
  }

  // Where a doc-order walk of the current segment meets its best keys: the
  // earliest leaf among the floor's best-bounded ones (the head of the
  // persisted bound order past the leaves outside the envelope, which hold
  // no domain doc; up to kHeadLeaves of them), or -1 when the floor holds
  // none. The column's bound order announces it; no traversal does.
  static constexpr int64_t kHeadLeaves = 64;
  int64_t headLeaf(int64_t floor) const {
    assert(primaryKeys.has_value());
    int64_t from = std::min(leavesOutsideValues.best, floor);
    int64_t to = std::min(floor, from + kHeadLeaves);
    int64_t head = -1;
    for (int64_t rank = from; rank < to; rank++) {
      int64_t leaf = primaryKeys->leafAt(rank);
      if (head < 0 || leaf < head) head = leaf;
    }
    return head;
  }

  // Whether the segment's keys improve with doc order, so a doc-order walk
  // meets them late and each doc it gathers beats the ones before it
  // (SegmentKeys::docOrderTrend past half its range).
  bool keysImproveWithDocOrder() const {
    return primaryKeys.has_value() && primaryKeys->docOrderTrend() > 0.5;
  }

  int64_t bottomPrimaryKey() const {
    assert(heapFull());
    return clauses[0].comparator->keyBatch()->slotKeys[pq.top().slot];
  }

  // Leaves bounded at or before the heap bottom's key (a tie may still win
  // on segdoc): once the heap is full, no doc in any other leaf of the
  // current segment can enter it, and the bottom only improves, so no
  // traversal from here on visits more leaves. Every leaf while the heap is
  // not full. A heap filled by earlier segments makes this the visit
  // bound a per-segment estimate cannot see.
  int64_t leavesWithinBottom() const {
    assert(primaryKeys.has_value());
    if (!heapFull()) return primaryKeys->leafCount();
    return primaryKeys->leavesBoundedAtOrBefore(bottomPrimaryKey());
  }

  // The current segment, whole, as the field-sort costs read it
  // (ExactDomainSortCosts::Rest) for a domain of `card` docs spanning
  // [firstDoc, lastDoc] with visit floor `floor` (predictedVisitFloor):
  // the leaves the bottom admits, those outside the domain's value
  // envelope, and the walk's lead. On keys that improve with doc order the
  // walk meets the best keys a share of the way through the domain's span,
  // and gathers that share of the inside leaves first; on keys in random
  // order the best keys' places say nothing of the ones the walk meets
  // before them. Every whole-segment field-sort decision prices this.
  ExactDomainSortCosts::Rest segmentRest(int64_t card, int64_t floor,
                                         int32_t firstDoc,
                                         int32_t lastDoc) const {
    assert(primaryKeys.has_value());
    int64_t leaves = primaryKeys->leafCount();
    int64_t inside = leaves - std::min(
        leaves, leavesOutsideValues.best + leavesOutsideValues.worst);
    int64_t lead = 0;
    int64_t head = lastDoc >= firstDoc && keysImproveWithDocOrder()
        ? headLeaf(floor) : -1;
    if (head >= 0) {
      int64_t leafSize = clauses[0].comparator->keyBatch()->leafBlockSize();
      int64_t firstLeaf = firstDoc / leafSize;
      int64_t spanLeaves = lastDoc / leafSize - firstLeaf + 1;
      lead = std::clamp<int64_t>(head - firstLeaf, 0, spanLeaves) * inside
          / spanLeaves;
    }
    return {card, leaves, floor, leavesWithinBottom(),
            leavesOutsideValues.best, inside, lead, topCount};
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

  // The rejected-doc path must stay small enough to inline into the collectTopK
  // loop (with the clause walk, copy loops, and heap sift inlined here, the
  // per-doc call overhead alone cost ~10% on single-field sort benchmarks), so
  // everything off that path is a LUXIR_NOINLINE helper.
  //
  // The external two-phase field-sort driver calls this before exact
  // verification. The heap must already be full: before that, every exact
  // match is needed to mature the bottom.
  bool LUXIR_INLINE competitiveCandidate(
      int32_t segment, int32_t docid, float score) {
    assert(heapFull());
    segdoc doc(segment, docid);
    SortDoc& bottom = pq.top();
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
    return cmp > 0;
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;
    if (topCount == 0) return;

    segdoc doc(segment, docid);
    if (pq.size() < (size_t)topCount) {
      warmup(doc, score);
      return;
    }

    SortDoc& bottom = pq.top();
    if (competitiveCandidate(segment, docid, score)) [[unlikely]] {
      admit(bottom, doc, score);
    }
  }

  struct CompetitiveRange {
    int32_t begin;
    int32_t end;
  };

  enum class BlockClass { COLLECT, SKIP_STRICT, SKIP_TIE };

  // The block-granular skip rules shared by every pruning driver (full
  // contract on nextCompetitiveRange): a strict bound loss always skips;
  // a bound tie skips only for a sole-clause sort whose segdoc tie-break
  // the block's first unseen doc loses.
  static BlockClass classifyKeyBlock(int32_t segment, int32_t firstUnseenDoc,
                                     int64_t best, int64_t bottomKey,
                                     segdoc bottomDoc, bool soleSort) {
    if (best > bottomKey) return BlockClass::SKIP_STRICT;
    if (best == bottomKey && soleSort
        && segdoc(segment, firstUnseenDoc) >= bottomDoc) {
      return BlockClass::SKIP_TIE;
    }
    return BlockClass::COLLECT;
  }

  // Driver-facing form: classify against the current heap bottom.
  BlockClass classifyBlock(int32_t segment, int32_t firstUnseenDoc,
                           int64_t best,
                           FieldComparator::KeyBatch* batch) const {
    assert(heapFull());
    return classifyKeyBlock(segment, firstUnseenDoc, best,
                            batch->slotKeys[pq.top().slot], pq.top().doc,
                            soleColumn != nullptr);
  }

  bool heapFull() const {
    return topCount > 0 && pq.size() == (size_t)topCount;
  }

  // Heap entries whose primary key is strictly below `bound`. When `bound`
  // lower-bounds every doc not yet seen, no later doc can displace them, so
  // they are final. Slots are minted densely, so slotKeys[0, size) is the
  // heap.
  int64_t countKeysBelow(int64_t bound,
                         FieldComparator::KeyBatch* batch) const {
    int64_t below = 0;
    for (size_t slot = 0; slot < pq.size(); slot++) {
      below += batch->slotKeys[slot] < bound;
    }
    return below;
  }

  struct KeyBlockPlan {
    FieldComparator::KeyBatch* batch = nullptr;
    int32_t blockSize = 0;
    int64_t blockCount = 0;
    int32_t leafSize = 0;
    int64_t leafCount = 0;
  };

  // Non-null batch only when ranking is active and the sole primary clause
  // offers order-independent masked leaf gathers (dense single-valued
  // numeric column) with two-level bounds.
  KeyBlockPlan maskedKeyBlockPlan() {
    KeyBlockPlan plan;
    FieldComparator::KeyBatch* batch;
    if (topCount > 0 && !disableKeyGatherForTests && soleColumn != nullptr
        && (batch = soleColumn->keyBatch()) != nullptr
        && batch->supportsMaskedLeafGather() && batch->leafBlockSize() != 0) {
      plan = {batch, batch->keyBlockSize(), batch->keyBlockCount(),
              batch->leafBlockSize(), batch->leafBlockCount()};
    }
    return plan;
  }

  // Competitive-range source for pruned collection. Returns the next doc range
  // at or after `from` that the primary clause's block key bounds cannot prove
  // noncompetitive. begin == PostingsReader::END means no remaining doc in this
  // segment can enter the heap. The range end is only a re-consult hint (the
  // next key-block boundary), never a claim about docs beyond it.
  //
  // Skip rules (smaller transformed key = better):
  // - A block whose best key is worse than the heap bottom's primary key can
  //   never contribute (secondary clauses cannot rescue a strictly worse
  //   primary).
  // - Equal-to-bottom blocks are skipped only for a sole-clause sort, and only
  //   when the block's first unseen segdoc loses the tie-break against the
  //   bottom's segdoc. The segdoc guard is required: collectors are reused
  //   across segments and segments are not always visited in order, so a later
  //   candidate does NOT always lose an equal-key tie.
  // - Missing-value sentinels are treated as ordinary keys (the comparator
  //   already ranks a real extreme and the sentinel as equal); a sentinel
  //   bottom simply proves nothing strictly, which is automatically safe.
  CompetitiveRange nextCompetitiveRange(int32_t segment, int32_t from) {
    FieldComparator::KeyBatch* batch = nullptr;
    int32_t blockSize = 0;
    if (topCount > 0 && !disableKeyGatherForTests
        && clauses[0].comparator != nullptr
        && (batch = clauses[0].comparator->keyBatch()) != nullptr) {
      blockSize = batch->keyBlockSize();
    }
    if (blockSize == 0) return candidateRange(segment, from);
    // Every leaf bounded before the key floor: no bound in this segment can
    // prove anything, so the rest of it is one competitive range, and range
    // ends need no re-consulting.
    if (leavesBeforeKeyFloor != 0
        && leavesBeforeKeyFloor == batch->leafBlockCount()) {
      return {from, PostingsReader::END};
    }

    int64_t block = (int64_t)from / blockSize;
    int64_t blockCount = batch->keyBlockCount();
    if (pq.size() < (size_t)topCount) {
      // No bound yet; re-consult at the next block boundary. Coarse-width
      // warm-up on purpose: nothing can prune, so leaf-width ranges would
      // only split producer calls.
      skipCount(SkipStats::fieldSortWarmupRanges);
      return {from, blockEnd(block, blockSize)};
    }
    int64_t bottomKey = batch->slotKeys[pq.top().slot];
    segdoc bottomDoc = pq.top().doc;
    bool soleSort = clauses.size() == 1;
    int32_t leafSize = descendsToLeaves() ? batch->leafBlockSize() : 0;
    int32_t doc = from;
    for (; block < blockCount; block++) {
      if (classifyKeyBlock(segment, doc, batch->blockBestKey(block),
                           bottomKey, bottomDoc, soleSort)
          == BlockClass::COLLECT) {
        // Descend: the coarse bound cannot prune this block, but individual
        // leaves still can (the coarse min lives in one leaf). All leaves
        // can refute a coarse COLLECT - equality skips consult the leaf's
        // own first unseen doc - so a refuted block falls through to the
        // next coarse block. A returned range extends across CONSECUTIVE
        // competitive leaves (maximal within the block), so runs where no
        // leaf can prune anything cost one producer call, not eight.
        if (leafSize == 0) break;
        int64_t leaf = (int64_t)doc / leafSize;
        int64_t leafLimit = std::min(
            ((block + 1) * (int64_t)blockSize + leafSize - 1) / leafSize,
            batch->leafBlockCount());
        for (; leaf < leafLimit; leaf++) {
          if (classifyKeyBlock(segment, doc, batch->leafBestKey(leaf),
                               bottomKey, bottomDoc, soleSort)
              == BlockClass::COLLECT) {
            int64_t endLeaf = leaf + 1;
            for (; endLeaf < leafLimit; endLeaf++) {
              if (classifyKeyBlock(
                      segment, (int32_t)(endLeaf * (int64_t)leafSize),
                      batch->leafBestKey(endLeaf), bottomKey, bottomDoc,
                      soleSort)
                  != BlockClass::COLLECT) {
                break;
              }
            }
            skipCount(SkipStats::fieldSortCompetitiveRanges);
            return {doc, blockEnd(endLeaf - 1, leafSize)};
          }
          skipCount(SkipStats::fieldSortLeavesSkipped);
          doc = blockEnd(leaf, leafSize);
        }
        skipCount(SkipStats::fieldSortBlocksSkipped);
        continue;
      }
      skipCount(SkipStats::fieldSortBlocksSkipped);
      doc = blockEnd(block, blockSize);
    }
    if (block >= blockCount) return {PostingsReader::END, PostingsReader::END};
    skipCount(SkipStats::fieldSortCompetitiveRanges);
    return {doc, blockEnd(block, blockSize)};
  }

  // Attribution only (no-op unless SkipStats::enabled): count blocks whose
  // best key strictly undercuts the final bottom (fieldSortIrreducibleBlocks)
  // and blocks a fresh traversal at the final bottom would still have to
  // collect under the full tie rules (fieldSortRequiredBlocks, the
  // equality-aware operational floor). Any correct bound-driven traversal
  // must inspect these blocks, in every visit order - the gap between the
  // visited-block counters and these is what reordering can save. Call after
  // a segment's collection completes.
  void recordSegmentSkipStats(int32_t segment) {
    if (!SkipStats::enabled || topCount == 0
        || pq.size() < (size_t)topCount || disableKeyGatherForTests) {
      return;
    }
    FieldComparator::KeyBatch* batch;
    if (clauses[0].comparator == nullptr
        || (batch = clauses[0].comparator->keyBatch()) == nullptr
        || batch->keyBlockSize() == 0) {
      return;
    }
    int32_t blockSize = batch->keyBlockSize();
    int64_t bottomKey = batch->slotKeys[pq.top().slot];
    segdoc bottomDoc = pq.top().doc;
    bool soleSort = clauses.size() == 1;
    for (int64_t block = 0, n = batch->keyBlockCount(); block < n; block++) {
      int64_t best = batch->blockBestKey(block);
      if (best < bottomKey) {
        SkipStats::fieldSortIrreducibleBlocks++;
      }
      if (classifyKeyBlock(segment, (int32_t)(block * (int64_t)blockSize),
                           best, bottomKey, bottomDoc, soleSort)
          == BlockClass::COLLECT) {
        SkipStats::fieldSortRequiredBlocks++;
      }
    }
    int32_t leafSize = batch->leafBlockSize();
    if (leafSize == 0) return;
    for (int64_t leaf = 0, n = batch->leafBlockCount(); leaf < n; leaf++) {
      int64_t best = batch->leafBestKey(leaf);
      if (best < bottomKey) {
        SkipStats::fieldSortIrreducibleLeaves++;
      }
      if (classifyKeyBlock(segment, (int32_t)(leaf * (int64_t)leafSize),
                           best, bottomKey, bottomDoc, soleSort)
          == BlockClass::COLLECT) {
        SkipStats::fieldSortRequiredLeaves++;
      }
    }
  }

  void collectWindow(int32_t segment, std::span<const int32_t> docs) {
    FieldComparator::KeyBatch* batch = nullptr;
    if (disableKeyGatherForTests || soleColumn == nullptr
        || (batch = soleColumn->keyBatch()) == nullptr) {
      for (int32_t doc : docs) {
        collect(segment, doc, 0.0f);
      }
      return;
    }

    hitCount += (int64_t)docs.size();
    if (topCount == 0) return;

    size_t i = 0;
    while (i < docs.size()) {
      size_t chunkStart = i;
      size_t chunkEnd = std::min(chunkStart + KEY_BUFFER_SIZE, docs.size());
      batch->gatherKeys(
          docs.subspan(chunkStart, chunkEnd - chunkStart),
          std::span<int64_t>(keys, chunkEnd - chunkStart));
      // Gathered-doc accounting advances per chunk so recordAdmission's
      // chunk-relative arithmetic stays doc-precise.
      if (SkipStats::enabled) {
        SkipStats::fieldSortDocsGathered += (int64_t)(chunkEnd - chunkStart);
      }
      admitChunk(segment, docs.subspan(chunkStart, chunkEnd - chunkStart),
                 std::span<const int64_t>(keys, chunkEnd - chunkStart), batch);
      i = chunkEnd;
    }
  }

  // Admit an already-gathered (docs, keys) batch. Docs ascend within a batch;
  // batches may arrive in any block order (segdoc is the tie key, and the
  // admission rules are order-independent).
  void admitGathered(int32_t segment, std::span<const int32_t> docs,
                     std::span<const int64_t> keys,
                     FieldComparator::KeyBatch* batch) {
    hitCount += (int64_t)docs.size();
    if (topCount == 0) return;
    if (SkipStats::enabled) {
      SkipStats::fieldSortDocsGathered += (int64_t)docs.size();
    }
    admitChunk(segment, docs, keys, batch);
  }

private:
  // Warmup fill then bottom-gated replacement over one gathered chunk.
  // Shared by the in-order window path (which gathers keys per chunk) and
  // the masked block driver (which gathers fused with the domain bits).
  // Pin the start alignment: the replacement loop is one tight compare per
  // doc, and a 32-byte shift from unrelated code moving cost resident
  // field sorts about 5% more cycles at identical instruction counts.
  [[gnu::aligned(64)]] void admitChunk(int32_t segment, std::span<const int32_t> docs,
                  std::span<const int64_t> keys,
                  FieldComparator::KeyBatch* batch) {
    size_t i = 0;
    while (i < docs.size() && pq.size() < (size_t)topCount) {
      int32_t slot = (int32_t)pq.size();
      ensureSlotCapacity(slot + 1);  // re-points batch->slotKeys on growth
      batch->slotKeys[slot] = keys[i];
      pq.insert(SortDoc(segdoc(segment, docs[i]), 0.0f, slot));
      recordAdmission(docs.size(), i);
      i++;
    }
    if (i == docs.size()) return;

    int64_t bottomKey = batch->slotKeys[pq.top().slot];
    for (; i < docs.size(); i++) {
      int64_t key = keys[i];
      segdoc doc(segment, docs[i]);
      if (key > bottomKey) continue;
      if (key == bottomKey && doc >= pq.top().doc) continue;

      SortDoc& bottom = pq.top();
      int32_t slot = bottom.slot;
      batch->slotKeys[slot] = key;
      bottom = SortDoc(doc, 0.0f, slot);
      pq.updateTop();
      bottomKey = batch->slotKeys[pq.top().slot];
      recordAdmission(docs.size(), i);
    }
  }

  // Attribution only: the gathered-doc index of this heap change, doc-precise
  // (fieldSortDocsGathered was bumped for the whole window on entry).
  static void recordAdmission(size_t windowSize, size_t i) {
    if (SkipStats::enabled) {
      SkipStats::fieldSortGatherAtLastAdmission =
          SkipStats::fieldSortDocsGathered - (int64_t)(windowSize - i) + 1;
    }
  }

  // Whether nextCompetitiveRange descends to leaves: priced by the docs the
  // leaves it would skip hold against classifying them
  // (ExactDomainSortCosts::walkDescends, over segmentRest of the walk's
  // card). Decided once per segment, against the heap at the first consult
  // that can prune.
  bool descendsToLeaves() {
    if (leafDescent == LeafDescent::UNDECIDED) {
      bool descend = false;
      if (forceLeafDescentForTests.has_value()) {
        descend = *forceLeafDescentForTests;
      } else if (int64_t card = segmentWalkCard > 0 ? segmentWalkCard
                                                    : segmentSourceCost;
                 primaryKeys.has_value() && card > 0) {
        descend = ExactDomainSortCosts::walkDescends(segmentRest(
            card, predictedVisitFloor(card), 0, segmentMaxDoc - 1));
      }
      leafDescent = descend ? LeafDescent::ON : LeafDescent::OFF;
    }
    return leafDescent == LeafDescent::ON;
  }

  static int32_t blockEnd(int64_t block, int32_t blockSize) {
    return (int32_t)std::min<int64_t>((block + 1) * blockSize,
                                      (int64_t)PostingsReader::END);
  }

  int32_t probeDeferral(int32_t from) {
    nextProbeDoc = (int32_t)std::min<int64_t>(
        (int64_t)from + CANDIDATE_PROBE_INTERVAL,
        (int64_t)PostingsReader::END);
    return nextProbeDoc;
  }

  // Candidate-union competitive source for primary clauses without block key
  // bounds (string ord comparators). Inactive consults return probe-interval
  // ranges so driver overhead stays near zero; once the ord interval is
  // narrow and cheap enough, the postings union activates and each consult
  // surfaces the next candidate doc as a one-doc range.
  CompetitiveRange candidateRange(int32_t segment, int32_t from) {
    if (candidatesDisabled || segmentPool == nullptr || topCount == 0
        || clauses[0].comparator == nullptr) {
      candidatesDisabled = true;
      return {from, PostingsReader::END};
    }
    if (pq.size() < (size_t)topCount) {
      return {from, probeDeferral(from)};
    }
    if (candidates == nullptr && from < nextProbeDoc) {
      return {from, nextProbeDoc};
    }
    FieldComparator::CompetitiveOrdState state;
    auto status = clauses[0].comparator->competitiveOrdInterval(
        state, pq.top().slot, pq.top().doc, segment, from,
        clauses.size() == 1);
    if (status == FieldComparator::OrdIntervalStatus::UNSUPPORTED) {
      candidatesDisabled = true;
      return {from, PostingsReader::END};
    }
    if (status == FieldComparator::OrdIntervalStatus::PENDING) {
      return {from, probeDeferral(from)};
    }
    if (state.lo > state.hi) {
      skipCount(SkipStats::fieldSortCandidateTerminations);
      return {PostingsReader::END, PostingsReader::END};
    }
    if (candidates == nullptr) {
      if (!tryActivateCandidates(state, from)) {
        return {from, probeDeferral(from)};
      }
    }
    if (state.lo != candidateLo || state.hi != candidateHi) {
      // The interval only narrows within a segment (the bottom improves
      // lexicographically and the segdoc-guard boundary drop is one-way).
      // narrow() speaks 0-based term ordinals (PostingsState::termOrdinal).
      bool narrowed = candidates->narrow(state.lo - 1, state.hi - 1);
      assert(narrowed);
      if (!narrowed) {
        candidatesDisabled = true;
        candidates = nullptr;
        return {from, PostingsReader::END};
      }
      candidateLo = state.lo;
      candidateHi = state.hi;
    }
    int32_t c = candidates->peekAdvance(from);
    if (c == PostingsReader::END) {
      skipCount(SkipStats::fieldSortCandidateTerminations);
      return {PostingsReader::END, PostingsReader::END};
    }
    return {c, c + 1};
  }

public:
  // The docFreq total a competitive interval of `termCount` terms may have
  // for the postings union to take over at docid `from` of a segment of
  // `maxDoc` docs whose source produces `sourceCost` docs, after `priorSource`
  // source docs of earlier segments; -1 when no total qualifies. Costs are
  // in walked source docs.
  //
  // By the next probe the heap has seen `seen` source docs: the earlier
  // segments' and those walked here (on a sparse filtered domain, its
  // cardinality, not the doc space, sets the pace). The bottom ranks k among
  // them, so the interval's terms c and docFreq total S shrink about as
  // 1/seen, and once active the union narrows behind the bottom as the walk
  // would. Waiting for the next probe, a walk of s source docs, captures about
  // c*s/seen fewer terms and skips the interval's S*s/sourceCost postings
  // over it: worth it while those cost more than the walk, i.e. while
  // K*S + CAP*c*sourceCost/seen > sourceCost. The union must also undercut
  // walking the `left` source docs to the end without narrowing further,
  // CAP*c + K*S*left/sourceCost <= left: the same form with `left` for
  // `seen`, binding once fewer are left than seen.
  static int64_t candidateBudget(int64_t sourceCost, int64_t priorSource,
                                 int32_t maxDoc, int32_t from,
                                 int64_t termCount) {
    if (sourceCost <= 0 || maxDoc <= 0 || from >= maxDoc) return -1;
    // A source cannot produce more docs than the segment holds; the clamp
    // also keeps every product below within 2^62.
    int64_t src = std::min<int64_t>(sourceCost, maxDoc);
    int64_t next = std::min<int64_t>(
        (int64_t)from + CANDIDATE_PROBE_INTERVAL, maxDoc);
    int64_t seen = priorSource + src * next / maxDoc;
    int64_t left = src * ((int64_t)maxDoc - from) / maxDoc;
    int64_t m = std::min(seen, left);
    int64_t capture = termCount * CANDIDATE_TERM_CAPTURE_COST;
    if (capture >= m) return -1;
    return src * (m - capture) / m / CANDIDATE_POSTING_COST;
  }

private:
  // The state-byte gate is free (interval width is known without touching
  // the dictionary); the docFreq total comes from candidateDocFreqs, which
  // reads each ord's docFreq at most once per segment and stops past the
  // budget.
  LUXIR_NOINLINE bool tryActivateCandidates(
      const FieldComparator::CompetitiveOrdState& state, int32_t from) {
    skipCount(SkipStats::fieldSortCandidateProbes);
    int64_t termCount = state.hi - state.lo + 1;
    if (termCount * (int64_t)sizeof(DocsOnlyEnum) > CANDIDATE_STATE_BYTES) {
      return false;
    }
    int64_t budget = candidateBudget(segmentSourceCost, priorSourceDocs,
                                     segmentMaxDoc, from, termCount);
    if (budget < 0) return false;
    // The interval only shrinks within a segment, so the first walk sized
    // candidateDocFreqs for every later one. Should that ever break, give up
    // on candidates for the segment, as candidateRange does when narrow()
    // refuses, rather than walk past the span.
    bool widened = candidateTerms != nullptr
        && termCount > (int64_t)candidateDocFreqs.size();
    assert(!widened);
    if (widened) {
      candidatesDisabled = true;
      return false;
    }
    bool profitable = candidateDocFreq(state, termCount, budget) <= budget;
#ifndef NDEBUG
    {
      // Check: the anchored totals reach the verdict a direct walk over the
      // interval does.
      MemPool::ScopeGuard scope(*segmentPool);
      TermsEnum direct(*segmentPool, *state.postingsReader, *state.fieldInfo);
      int64_t sum = 0;
      for (int64_t ord = state.lo; ord <= state.hi && sum <= budget; ord++) {
        direct.seekOrd(ord - 1);
        sum += direct.docFreq();
      }
      assert((sum <= budget) == profitable);
    }
#endif
    if (!profitable) return false;
    TermsEnum& te = *candidateTerms;
    ChunkedArray<TermsEnum::PostingsState> states(*segmentPool, (size_t)termCount);
    states.reserve((size_t)termCount);
    for (int64_t i = 0; i < termCount; i++) {
      te.seekOrd(state.lo - 1 + i);
      states.push_back(te.postingsState());
    }
    auto enums = segmentPool->make_span<DocsOnlyEnum*>((size_t)termCount);
    std::fill(enums.begin(), enums.end(), nullptr);
    auto heap = segmentPool->make_span<uint64_t>((size_t)termCount);
    auto windowBits = segmentPool->make_span<uint64_t>(
        (size_t)UnionHeapScorer::WINDOW_WORDS);
    candidates = segmentPool->make<UnionHeapScorer>(
        states.view(), enums, heap, windowBits, *segmentPool, segmentMaxDoc, 0.0f);
    candidateLo = state.lo;
    candidateHi = state.hi;
    // narrow() speaks 0-based term ordinals (PostingsState::termOrdinal).
    candidates->narrow(candidateLo - 1, candidateHi - 1);
    skipCount(SkipStats::fieldSortCandidateActivations);
    return true;
  }

  // The docFreq total of the `count` ords nearest the anchored end of
  // `state`'s interval, or, when that passes `budget`, a partial total that
  // already does.
  int64_t candidateDocFreq(const FieldComparator::CompetitiveOrdState& state,
                           int64_t count, int64_t budget) {
    assert(count > 0);
    if (count > candidateReach
        && (candidateReach == 0
            || candidateDocFreqs[(size_t)candidateReach - 1] <= budget)) {
      extendCandidateDocFreqs(state, count, budget);
    }
    return candidateDocFreqs[(size_t)std::min(count, candidateReach) - 1];
  }

  // Extends candidateDocFreqs outward from the anchor, one term block's
  // stats decode at a time, until it covers `count` ords or its total passes
  // `budget`. The first call sizes it: no later interval is wider.
  LUXIR_NOINLINE void extendCandidateDocFreqs(
      const FieldComparator::CompetitiveOrdState& state, int64_t count,
      int64_t budget) {
    assert(state.descending ? state.hi == state.nTerms : state.lo == 1);
    if (candidateTerms == nullptr) {
      candidateTerms = segmentPool->make<TermsEnum>(
          *segmentPool, *state.postingsReader, *state.fieldInfo);
      candidateDocFreqs = segmentPool->make_span<int64_t>((size_t)count);
    }
    assert(count <= (int64_t)candidateDocFreqs.size());
    int64_t reach = candidateReach;
    int64_t total = reach == 0 ? 0 : candidateDocFreqs[(size_t)reach - 1];
    while (reach < count && total <= budget) {
      // 0-based ord of the next term outward from the anchor.
      int64_t ord = state.descending ? state.nTerms - 1 - reach : reach;
      auto [firstOrd, docFreqs] = candidateTerms->docFreqBlock(ord);
      int64_t i = ord - firstOrd;
      int64_t step = state.descending ? -1 : 1;
      for (; i >= 0 && i < (int64_t)docFreqs.size() && reach < count;
           i += step) {
        total += docFreqs[(size_t)i];
        candidateDocFreqs[(size_t)reach++] = total;
      }
    }
    if (SkipStats::enabled) {
      SkipStats::fieldSortCandidateDocFreqs += reach - candidateReach;
    }
    candidateReach = reach;
  }

  // Grow every clause's slot storage in lockstep, doubling up to topCount.
  // Must be called (via ensureSlotCapacity) before the first write to a new
  // slot; that includes direct KeyBatch::slotKeys writes, which growSlots
  // re-points at the resized storage.
  LUXIR_NOINLINE void growSlotStorage(int64_t required) {
    assert(required <= topCount);
    int64_t capacity = std::min(topCount,
        std::max({required, slotCapacity * 2, (int64_t)64}));
    for (auto& clause : clauses) {
      if (clause.comparator != nullptr) {
        clause.comparator->growSlots((int32_t)capacity);
      } else if (clause.expr != nullptr) {
        resizeExact(clause.expr->slots, (size_t)capacity);
      }
    }
    slotCapacity = capacity;
  }

  void ensureSlotCapacity(int64_t required) {
    if (required > slotCapacity) growSlotStorage(required);
  }

  // Private because writing a slot is only safe after ensureSlotCapacity has
  // covered it; every caller is on a path that just did so (or reuses a slot
  // already in the heap).
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

  LUXIR_NOINLINE void warmup(segdoc doc, float score) {
    int32_t slot = (int32_t)pq.size();
    ensureSlotCapacity(slot + 1);
    copy(slot, doc, score);
    pq.insert(SortDoc(doc, score, slot));
    if (pq.size() == (size_t)topCount) setBottom();
  }

  LUXIR_NOINLINE int compareCurrentDoc(const SortDoc& bottom, segdoc doc, float score) const {
    SortDoc candidate(doc, score, -1);
    return compare(bottom, candidate, CompareSource::CURRENT_DOC);
  }

  LUXIR_NOINLINE void admit(SortDoc& bottom, segdoc doc, float score) {
    copy(bottom.slot, doc, score);
    bottom = SortDoc(doc, score, bottom.slot);
    pq.updateTop();
    setBottom();
  }

  void setBottom() {
    int32_t slot = pq.top().slot;
    for (auto& clause : clauses) {
      if (clause.comparator != nullptr) clause.comparator->setBottom(slot);
    }
  }

public:

  void merge(FieldSortCollector& other) {
    // Self-merge would iterate a span that insert() can reallocate out from under us.
    assert(this != &other);
    hitCount += other.hitCount;
    needsSort = true;

    for (const SortDoc& otherDoc : other.pq.span()) {
      if (pq.size() < (size_t)topCount) {
        int32_t slot = (int32_t)pq.size();
        ensureSlotCapacity(slot + 1);
        copy(slot, other, otherDoc.slot, otherDoc.doc);
        pq.insert(SortDoc(otherDoc.doc, otherDoc.score, slot));
      } else {
        SortDoc& bottom = pq.top();
        if (compare(bottom, otherDoc, CompareSource::OTHER, &other) > 0) {
          copy(bottom.slot, other, otherDoc.slot, otherDoc.doc);
          bottom = SortDoc(otherDoc.doc, otherDoc.score, bottom.slot);
          pq.updateTop();
        }
      }
    }
  }

  int64_t totalHits() const {
    return hitCount;
  }

  uint64_t size() const {
    return pq.size();
  }

  std::span<SortDoc> sort() {
    std::span<SortDoc> docs = pq.span();
    if (needsSort) {
      FieldSortComparatorFunctor comp(this);
      std::sort_heap(docs.begin(), docs.end(), comp);
      needsSort = false;
    }
    return docs;
  }

  std::span<SortDoc> scoreDocs() {
    return pq.span();
  }
};

inline bool FieldSortCollector::FieldSortComparatorFunctor::operator()(
    const SortDoc& docA, const SortDoc& docB) const {
  return collector->compare(docA, docB) < 0;
}

} // namespace luxir
