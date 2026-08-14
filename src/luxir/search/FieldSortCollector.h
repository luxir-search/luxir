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
      }
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
      const ValueResult& result = current(doc, score);
      ExprSlot& target = expr->slots[(size_t)slot];
      target.valid = result.valid;
      if (!result.valid) return;
      if (expr->isDouble) target.value.doubleValue = result.doubleValue;
      else target.value.intValue = result.intValue;
    }

    void copyFrom(int32_t slot, const RuntimeClause& other, int32_t otherSlot) {
      expr->slots[(size_t)slot] = other.expr->slots[(size_t)otherSlot];
    }

    int compareValues(const ExprSlot& left, const ExprSlot& right) const {
      if (left.valid != right.valid) return left.valid ? -1 : 1;
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
      const ValueResult& result = current(doc, score);
      ExprSlot candidate;
      candidate.valid = result.valid;
      if (result.valid) {
        if (expr->isDouble) candidate.value.doubleValue = result.doubleValue;
        else candidate.value.intValue = result.intValue;
      }
      return compareValues(expr->slots[(size_t)left], candidate);
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
  // competitive ord interval; it and every allocation behind it live in
  // segmentPool, which the dispatching op owns for the segment's lifetime.
  static constexpr int32_t CANDIDATE_PROBE_INTERVAL = 4096;
  static constexpr int64_t CANDIDATE_COST_RATIO = 8;
  static constexpr int64_t CANDIDATE_STATE_BYTES = 8 << 20;
  // Docs-equivalent charge per captured term: the activation walk pays a
  // dictionary seek, a stats decode, and a postings-state capture per term,
  // and the union pays heap traffic per term after that. Charging it keeps
  // wide intervals from activating against small filtered domains.
  static constexpr int64_t CANDIDATE_TERM_CAPTURE_COST = 16;
  MemPool* segmentPool = nullptr;
  UnionHeapScorer* candidates = nullptr;
  int64_t candidateLo = 0;
  int64_t candidateHi = -1;
  int64_t segmentSourceCost = 0;
  int32_t segmentMaxDoc = 0;
  bool leafDescentEnabled = false;
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
    nextProbeDoc = 0;
    candidatesDisabled = false;
    segmentSourceCost = sourceCost;
    segmentMaxDoc = reader != nullptr ? reader->maxDoc() : 0;
    for (auto& clause : clauses) {
      clause.resetCache();
      if (clause.comparator != nullptr) {
        clause.comparator->setSegment(segment, reader);
      }
    }
    // Leaf descent in nextCompetitiveRange only pays when the expected leaf
    // visit floor (ceil(k * maxDoc / sourceCost) leaves) is MATERIALLY
    // sub-saturating. At saturation every leaf classifies COLLECT under the
    // final bottom, so descending would only split producer calls eight ways
    // and classify eight bounds per block for nothing - and near saturation
    // (measured at floor/leafCount ~0.99: a reproducible ~30% regression)
    // the few skippable leaves fragment producer ranges without paying for
    // the classifications. The 2x margin keeps the whole win at 1% and off
    // through the boundary band.
    leafDescentEnabled = false;
    FieldComparator::KeyBatch* batch;
    if (topCount > 0 && sourceCost > 0 && clauses[0].comparator != nullptr
        && (batch = clauses[0].comparator->keyBatch()) != nullptr
        && batch->leafBlockSize() != 0) {
      int64_t depth = (topCount * (int64_t)segmentMaxDoc + sourceCost - 1)
          / sourceCost;
      leafDescentEnabled = 2 * depth < batch->leafBlockCount();
    }
    if (topCount > 0 && pq.size() == (size_t)topCount) setBottom();
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
  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;
    if (topCount == 0) return;

    segdoc doc(segment, docid);
    if (pq.size() < (size_t)topCount) {
      warmup(doc, score);
      return;
    }

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
    if (cmp > 0) [[unlikely]] {
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
    int32_t leafSize = leafDescentEnabled ? batch->leafBlockSize() : 0;
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
  void admitChunk(int32_t segment, std::span<const int32_t> docs,
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

  // The state-byte gate is free (interval width is known without touching
  // the dictionary); the docFreq sum costs one stats decode per 128 terms
  // and aborts at the profitability limit. The capture walk uses its own
  // TermsEnum: the comparator's leaf enum backs setBottom/copy caching and
  // must not be repositioned here.
  LUXIR_NOINLINE bool tryActivateCandidates(
      const FieldComparator::CompetitiveOrdState& state, int32_t from) {
    int64_t termCount = state.hi - state.lo + 1;
    if (segmentSourceCost < 0
        || termCount * (int64_t)sizeof(DocsOnlyEnum) > CANDIDATE_STATE_BYTES) {
      return false;
    }
    // Prorate the source estimate by the fraction of the segment left, then
    // require the union (postings plus per-term capture) to undercut it by
    // the cost ratio. The exhaustive baseline on a sparse filtered domain is
    // its cardinality, not the doc space.
    int64_t remaining = segmentMaxDoc <= 0 ? 0
        : segmentSourceCost * ((int64_t)segmentMaxDoc - from) / segmentMaxDoc;
    int64_t limit = remaining / CANDIDATE_COST_RATIO;
    int64_t sum = termCount * CANDIDATE_TERM_CAPTURE_COST;
    if (sum > limit) return false;
    TermsEnum te(*segmentPool, *state.postingsReader, *state.fieldInfo);
    for (int64_t ord = state.lo; ord <= state.hi; ord++) {
      te.seekOrd(ord - 1);
      sum += te.docFreq();
      if (sum > limit) return false;
    }
    auto states =
        segmentPool->make_span<TermsEnum::PostingsState>((size_t)termCount);
    for (int64_t i = 0; i < termCount; i++) {
      te.seekOrd(state.lo - 1 + i);
      states[(size_t)i] = te.postingsState();
    }
    auto enums = segmentPool->make_span<DocsOnlyEnum*>((size_t)termCount);
    std::fill(enums.begin(), enums.end(), nullptr);
    auto heap = segmentPool->make_span<uint64_t>((size_t)termCount);
    auto windowBits = segmentPool->make_span<uint64_t>(
        (size_t)UnionHeapScorer::WINDOW_WORDS);
    candidates = segmentPool->make<UnionHeapScorer>(
        states, enums, heap, windowBits, *segmentPool, segmentMaxDoc, 0.0f);
    candidateLo = state.lo;
    candidateHi = state.hi;
    // narrow() speaks 0-based term ordinals (PostingsState::termOrdinal).
    candidates->narrow(candidateLo - 1, candidateHi - 1);
    skipCount(SkipStats::fieldSortCandidateActivations);
    return true;
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
