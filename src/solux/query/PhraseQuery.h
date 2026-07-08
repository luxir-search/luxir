#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "ImpactsIndex.h"
#include "Query.h"
#include "solux/reader/NormsReader.h"

namespace solux {

class PhraseQuery final : public Query {
  std::string_view field;
  std::span<std::string_view> terms;
  std::span<const int32_t> positions;
public:
  PhraseQuery(std::string_view field, std::span<std::string_view> terms, std::span<const int32_t> positions) : field(field),
                                                                                                         terms(terms),
                                                                                                         positions(
                                                                                                                 positions) {
    assert(terms.size() == positions.size());
    assert(terms.size() >= 2);
  }

  [[nodiscard]] std::string_view getField() const {
    return field;
  }

  [[nodiscard]] std::span<std::string_view> getTerms() const {
    return terms;
  }

  [[nodiscard]] std::span<const int32_t> getPositions() const {
    return positions;
  }

  Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<PhraseQuery::Weight>(context, *this, flags);
  }


  class Weight final : public Query::Weight {
    PhraseQuery& query;
    CachedFieldInfo* cachedFieldInfo;
    std::span<CachedTermInfo*> cachedTermInfos;
    Similarity::BM25Scorer* simScorer = nullptr;
  public:
    Weight(Query::Context& context, PhraseQuery& query, int32_t flags)
            : Query::Weight(context, flags), query(query) {
      bool needScores = (flags & NEED_SCORES) != 0;
      // Filter-style phrases match normally but always score 0.
      if (!needScores) traits |= IS_CONSTANT_SCORING;
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
      if (cachedFieldInfo == nullptr) {
        return;
      }
      cachedTermInfos = context.pool.make_span<CachedTermInfo*>(query.getTerms().size());
      Similarity similarity;
      double idf = 0.0;
      for (int i = 0; i < cachedTermInfos.size(); i++) {
        cachedTermInfos[i] = context.getCachedTerminfo(*cachedFieldInfo, query.getTerms()[i]);
        if (cachedTermInfos[i] == nullptr) {
          // can't match if a term doesn't exist
          cachedFieldInfo = nullptr;
          return;
        }
        idf += similarity.idf(cachedFieldInfo->fieldStats, cachedTermInfos[i]->termStats);
      }
      // Position matching does not need BM25; build it only for scoring clauses.
      if (needScores) {
        simScorer = context.pool.make<Similarity::BM25Scorer>(
                similarity.getScorer(1.0f, cachedFieldInfo->fieldStats, (float) idf));
      }
    }

    Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      if (cachedFieldInfo == nullptr) {
        return nullptr;
      }
      // segInfos is per-segment; cachedTermInfos is per-term.
      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
      if (segFieldInfo == nullptr) {
        return nullptr;
      }
      // Repeated terms share one postings enum, so a duplicated term's doc
      // and position blocks decode once. The doc conjunction needs no special
      // handling for the aliases (doNext's strict-advance guard skips an enum
      // already at the target); position verification reads the shared
      // positions through per-slot cursors (see Scorer::RepeatGroup).
      auto docsEnums = targetPool.make_span<DocsEnum*>(cachedTermInfos.size());
      auto terms = query.getTerms();
      int32_t distinctCount = 0;
      bool hasRepeats = false;
      for (int i = 0; i < cachedTermInfos.size(); i++) {
        int32_t first = (int32_t) i;
        if (!Scorer::disableRepeatDedupForTests) {
          for (int32_t j = 0; j < (int32_t) i; j++) {
            if (terms[(size_t) j] == terms[(size_t) i]) {
              first = j;
              break;
            }
          }
        }
        if (first != (int32_t) i) {
          docsEnums[i] = docsEnums[(size_t) first];
          hasRepeats = true;
          continue;
        }
        distinctCount++;
        docsEnums[i] = cachedTermInfos[i]->useDocsEnum(targetPool, segment);
        if (docsEnums[i] == nullptr) {
          // term doesn't exist in this segment
          return nullptr;
        }
      }

      // Execute the phrase over its terms in increasing docFreq order (Lucene
      // sorts exact phrase postings the same way): docsEnums[0] leads both the
      // doc conjunction and the position walk, and phrases often start with
      // their most common word ("the incredibles"), making text order a poor
      // lead.  The query's public term order and idf are untouched; each
      // position offset travels with its term, and the position algorithm
      // assigns no meaning to offset order.
      auto positions = targetPool.make_span<int32_t>(docsEnums.size());
      {
        std::vector<int32_t> order(docsEnums.size());
        for (size_t i = 0; i < order.size(); i++) {
          order[i] = (int32_t) i;
        }
        if (!PhraseQuery::Scorer::disableSortForTests) {
          std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
            auto* ea = docsEnums[(size_t) a];
            auto* eb = docsEnums[(size_t) b];
            if (ea->numDocs() != eb->numDocs()) return ea->numDocs() < eb->numDocs();
            if (ea->totalTermFreq() != eb->totalTermFreq()) {
              return ea->totalTermFreq() < eb->totalTermFreq();
            }
            return a < b;  // deterministic; keeps repeated terms in text order
          });
        }
        std::vector<DocsEnum*> byOrder(docsEnums.begin(), docsEnums.end());
        for (size_t k = 0; k < order.size(); k++) {
          docsEnums[k] = byOrder[(size_t) order[k]];
          positions[k] = query.getPositions()[(size_t) order[k]];
        }
      }

      // Map slots that share an enum onto repeat groups for the position walk.
      std::span<const int32_t> slotGroup{};
      std::span<Scorer::RepeatGroup> groups{};
      if (hasRepeats) {
        auto sg = targetPool.make_span<int32_t>(docsEnums.size());
        std::fill(sg.begin(), sg.end(), -1);
        int32_t numGroups = 0;
        for (size_t i = 0; i < docsEnums.size(); i++) {
          if (sg[i] >= 0) continue;
          int32_t gid = -1;
          for (size_t j = i + 1; j < docsEnums.size(); j++) {
            if (docsEnums[j] == docsEnums[i]) {
              if (gid < 0) {
                gid = numGroups++;
                sg[i] = gid;
              }
              sg[j] = gid;
            }
          }
        }
        groups = targetPool.make_span<Scorer::RepeatGroup>((size_t) numGroups);
        for (size_t i = 0; i < docsEnums.size(); i++) {
          if (sg[i] >= 0) {
            groups[(size_t) sg[i]].docsEnum = docsEnums[i];
          }
        }
        slotGroup = sg;
      }

      // Position matching does not need norms when score() is never read.
      NormsReader* normsReader = (inputFlags & NEED_SCORES) != 0
              ? targetPool.make<NormsReader>(segment.postingsReader(), *segFieldInfo)
              : nullptr;

      // Per-term block impact indexes evaluated with the PHRASE's scorer:
      // phraseFreq <= each term's freq in a doc, so every term's frontier
      // bounds the phrase score, and the min across terms bounds any doc
      // range.  These serve getMaxScore/advanceShallow when a block-max
      // parent drives the phrase as a clause; the phrase's own single-phase
      // pruning is the per-doc bound in doMatches (a range-skip loop here was
      // measured a net loss - the range bounds are too loose to fire).
      // One index per DISTINCT enum: getMaxScore/advanceShallow take a min
      // over the entries, so a duplicated term contributes once.
      std::span<ImpactsIndex> impacts;
      if (simScorer != nullptr && normsReader != nullptr) {
        auto built = targetPool.make_span<ImpactsIndex>((size_t) distinctCount);
        bool allBuilt = true;
        size_t n = 0;
        for (size_t i = 0; i < docsEnums.size(); i++) {
          bool seen = false;
          for (size_t j = 0; j < i && !seen; j++) {
            seen = docsEnums[j] == docsEnums[i];
          }
          if (seen) continue;
          built[n].build(targetPool, *docsEnums[i], *simScorer, 1.0f);
          allBuilt &= !built[n].empty();
          n++;
        }
        assert(n == (size_t) distinctCount);
        if (allBuilt) {
          impacts = built;  // a term without impact data (pulsed) disables bounding
        }
      }

      return targetPool.make<PhraseQuery::Scorer>(targetPool, docsEnums, positions, normsReader,
                                                  simScorer, impacts, slotGroup, groups);
    }

    // A phrase matches a subset of the docs containing its rarest term, so its
    // cardinality cost is the min over the terms' per-segment doc counts (an
    // upper bound, like Lucene's phrase weight). This is only the cardinality
    // axis: confirming a phrase is far more work per candidate than a term (the
    // position walk), which two-phase iteration / match cost would model.
    class Supplier final : public Query::ScorerSupplier {
      PhraseQuery::Weight& weight;
      IndexReader::Segment& segment;
    public:
      Supplier(PhraseQuery::Weight& weight, IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override {
        if (weight.cachedFieldInfo == nullptr) return 0;
        int64_t minCost = -1;
        for (auto* termInfo : weight.cachedTermInfos) {
          auto* docsEnum = termInfo->docsEnums[segment.ord];
          if (docsEnum == nullptr) return 0;  // a term absent here -> phrase matches nothing
          int64_t c = docsEnum->numDocs();
          if (minCost < 0 || c < minCost) minCost = c;
        }
        return minCost < 0 ? 0 : minCost;
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        unused(leadCost);
        return weight.createScorer(targetPool, segment);
      }
    };

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool, IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment);
    }
  };


  class Scorer final : public Query::Scorer {
  public:
    // One duplicated term's positions for the current doc, drained once and
    // read by every slot of that term through its own cursor. A shared
    // advancePosition cursor would be wrong: when the base candidate advances
    // by less than the gap between two offsets of the same term, the lower
    // offset's next target lies behind where the shared cursor already moved.
    struct RepeatGroup {
      DocsEnum* docsEnum = nullptr;
      int32_t* buf = nullptr;  // absolute positions of the current doc + END sentinel
      int32_t cap = 0;
      int32_t count = 0;       // termFreq of the current doc
      int32_t filled = 0;      // positions pulled so far; buf[count] is the sentinel
    };

  private:
    std::span<DocsEnum*> docsEnums;
    std::span<const int32_t> positions;
    // Repeated-term support; groups is empty (and the fields dormant) for the
    // common no-repeats phrase, whose verification path is unchanged.
    MemPool* pool;
    std::span<const int32_t> slotGroup;  // slot -> repeat group, -1 = own enum
    std::span<RepeatGroup> groups;
    std::span<int32_t> slotCursor;       // per-slot index into its group's buf
    bool hasRepeats = false;
    // Both absent when scores are not needed; score() is 0.
    std::optional<NormsReader::Iterator> normsIter;
    Similarity::BM25Scorer* simScorer;
    // One impact index per term, evaluated with the phrase's simScorer; empty
    // when not scoring or when any term lacks impact data.
    std::span<ImpactsIndex> impacts;
    float minCompetitiveScore = 0.0f;
    int32_t shallowTarget = -1;

    int32_t docid = -1;
    int32_t pos = -1;    // position of last match, or END if no more matches.
    int32_t freq = 0;
    int32_t largestPossiblePos;   // largest possible position for a match
    int32_t checkedDocid = -1;
    bool checkedMatch = false;
    float matchCostEstimate = 0.0f;
    // One norm read per doc: the pre-position bound in doMatches() and score()
    // both need it, and the sparse norms iterator is strict-advance (it cannot
    // re-advance to the doc it already sits on).  Flat norms bypass the cache.
    const uint8_t* flatNormsBase = nullptr;
    int32_t cachedNormDoc = -1;
    uint8_t cachedNorm = 0;
#ifndef NDEBUG
    int32_t protocol = 0;
#endif


    // internal utility method where first scorer has already been advanced and is equal to the target.
    int32_t doNext(int32_t target) {
      auto* firstEnum = docsEnums[0];

      outer:
      for (;;) {
        for (int j = 1; j < docsEnums.size(); j++) {
          // docsEnums[j] may already sit on target (firstEnum landed exactly on a
          // doc it was already at); advance() is strict, so only advance the ones
          // that are behind (same guard as ConjunctionScorer).
          if (docsEnums[j]->docId() < target) {
            int32_t id = docsEnums[j]->advance(target);
            assert(id >= target);
            if (id > target) {
              // TODO: explicitly handle END here for faster termination?
              target = firstEnum->advance(id);
              goto outer;  // could perhaps replace with "j=0; continue;" but that seems potentially worse?
            }
          }
        }
        // if we made it through the loop, all docsenum matched (maybe at END)
        docid = target;
        return docid;
      }
      // unreachable
    }

    // internal utility method where first enum has already had position advanced.
    // what is passed here is the hypothetical position of the phrase, not the actual position of the first DocsEnum.
    // i.e. pass (docsEnum->advancePosition(positions[0]) - positions[0])
    int32_t doNextPosition(int32_t target) {
      outer:
      for (;;) {
        if (target > largestPossiblePos) {
          pos = PostingsReader::END;
          return PostingsReader::END;
        }

        for (int j = 1; j < docsEnums.size(); j++) {
          int32_t adjustedTarget = target + positions[j];
          // prev comparison to largestPossiblePos should keep adjustedTarget from overflowing.
          int32_t p = docsEnums[j]->advancePosition((int32_t) adjustedTarget);
          assert(p >= adjustedTarget);
          if (p > adjustedTarget) {
            // we overshot, so we need to advance the first enum and try again
            target = p - positions[j];
            if (target > largestPossiblePos) {
              pos = PostingsReader::END;
              return PostingsReader::END;
            }
            adjustedTarget = target + positions[0];
            p = docsEnums[0]->advancePosition(adjustedTarget);
            target = p - positions[0];
            goto outer;
          }
        }
        // if we made it through the loop, all the positions matched!
        pos = target;
        freq++;
        return pos;
      }
      // unreachable
    }

    // Per-doc reset only: positions are pulled lazily as the cursors need
    // them (an eager drain measured 7% slower on repeat stopword phrases -
    // failed candidates abandon after a few positions, and eager decoding of
    // every candidate's full tf threw that away). buf[count] is the END
    // sentinel, placed up front so cursor scans always terminate.
    void resetRepeatGroups() {
      for (auto& group : groups) {
        group.count = group.docsEnum->termFreq();
        if (group.count >= group.cap) {
          group.cap = std::max(group.count + 1, group.cap * 2);
          group.buf = pool->make_arr<int32_t>((size_t) group.cap);
        }
        group.filled = 0;
        group.buf[group.count] = PostingsReader::END;
      }
      std::fill(slotCursor.begin(), slotCursor.end(), 0);
    }

    // advancePosition for a slot: direct for a slot that owns its enum,
    // cursor-over-shared-buffer for a repeat slot, filling the buffer from
    // the enum on demand. Each position is decoded once no matter how many
    // slots read it. The sentinel bounds the scan (targets never exceed
    // largestPossiblePos < END).
    int32_t repeatSlotAdvance(int32_t slot, int32_t target) {
      int32_t g = slotGroup[(size_t) slot];
      if (g < 0) {
        return docsEnums[(size_t) slot]->advancePosition(target);
      }
      RepeatGroup& group = groups[(size_t) g];
      int32_t idx = slotCursor[(size_t) slot];
      for (;;) {
        while (idx >= group.filled && group.filled < group.count) {
          group.buf[group.filled++] = group.docsEnum->nextPosition();
        }
        if (group.buf[idx] >= target) {
          break;
        }
        idx++;
      }
      slotCursor[(size_t) slot] = idx;
      return group.buf[idx];
    }

    // nextPosition for a slot, mirroring repeatSlotAdvance's cursor semantics
    // (the cursor rests on the last returned position).
    int32_t repeatSlotNext(int32_t slot) {
      int32_t g = slotGroup[(size_t) slot];
      if (g < 0) {
        return docsEnums[(size_t) slot]->nextPosition();
      }
      RepeatGroup& group = groups[(size_t) g];
      int32_t idx = slotCursor[(size_t) slot];
      if (idx >= group.count) {
        return PostingsReader::END;  // resting on the sentinel already
      }
      idx++;
      while (idx >= group.filled && group.filled < group.count) {
        group.buf[group.filled++] = group.docsEnum->nextPosition();
      }
      slotCursor[(size_t) slot] = idx;
      return group.buf[idx];
    }

    // doNextPosition for phrases with repeated terms: identical walk, with
    // per-slot cursors standing in for per-enum position cursors.
    int32_t doNextPositionRepeats(int32_t target) {
      outer:
      for (;;) {
        if (target > largestPossiblePos) {
          pos = PostingsReader::END;
          return PostingsReader::END;
        }

        for (int j = 1; j < docsEnums.size(); j++) {
          int32_t adjustedTarget = target + positions[j];
          int32_t p = repeatSlotAdvance(j, adjustedTarget);
          assert(p >= adjustedTarget);
          if (p > adjustedTarget) {
            target = p - positions[j];
            if (target > largestPossiblePos) {
              pos = PostingsReader::END;
              return PostingsReader::END;
            }
            adjustedTarget = target + positions[0];
            p = repeatSlotAdvance(0, adjustedTarget);
            target = p - positions[0];
            goto outer;
          }
        }
        pos = target;
        freq++;
        return pos;
      }
      // unreachable
    }

    int32_t doApproximationNext() {
      return doNext(docsEnums[0]->next());
    }

    int32_t doApproximationAdvance(int32_t target) {
      return doNext(docsEnums[0]->advance(target));
    }

    int64_t lookupNorm(int32_t doc) {
      if (flatNormsBase != nullptr) {
        return flatNormsBase[doc];
      }
      if (doc != cachedNormDoc) {
        int32_t normDoc = normsIter->advance(doc);
        assert(normDoc == doc);
        unused(normDoc);
        cachedNormDoc = doc;
        cachedNorm = normsIter->value();
      }
      return cachedNorm;
    }

    bool doMatches() {
      if (docid == checkedDocid) {
        return checkedMatch;
      }
      checkedDocid = docid;
      checkedMatch = false;
      freq = 0;
      if (docid == PostingsReader::END) {
        pos = PostingsReader::END;
        return false;
      }
      if (countMatchesForTests) {
        matchCallsForTests++;
      }
      // Reject this candidate before touching its positions when even the
      // largest possible phrase freq cannot reach the collector's threshold
      // (Lucene PhraseScorer.matches parity): phraseFreq <= min(term tf), and
      // BM25 is monotone in freq.  A nonzero threshold is the caller's
      // guarantee that sub-threshold matches are droppable, so a two-phase
      // parent that forwards its threshold opts into pruned matches; without a
      // threshold, match semantics are unchanged.
      if (minCompetitiveScore > 0.0f && simScorer != nullptr && !disableDocBoundForTests) {
        int32_t maxFreq = docsEnums[0]->termFreq();
        for (size_t j = 1; j < docsEnums.size() && maxFreq > 1; j++) {
          maxFreq = std::min(maxFreq, docsEnums[j]->termFreq());
        }
        if (simScorer->score((float) maxFreq, lookupNorm(docid)) < minCompetitiveScore) {
          skipCount(SkipStats::phraseBoundRejects);
          pos = PostingsReader::END;
          return false;
        }
      }
      skipCount(SkipStats::phraseVerifies);
      // startPositions is idempotent once positioned (posOrd == posOrdStart),
      // so alias slots of a repeated term are harmless no-ops here.
      for (auto* docsEnum: docsEnums) {
        docsEnum->startPositions();
      }
      if (!hasRepeats) {
        checkedMatch = doNextPosition(docsEnums[0]->advancePosition(positions[0]) - positions[0]) != PostingsReader::END;
      } else {
        resetRepeatGroups();
        checkedMatch = doNextPositionRepeats(
            repeatSlotAdvance(0, positions[0]) - positions[0]) != PostingsReader::END;
      }
      return checkedMatch;
    }

#ifndef NDEBUG
    void markSinglePhase() {
      assert(protocol != 2);
      protocol = 1;
    }

    void markTwoPhase() {
      assert(protocol != 1);
      protocol = 2;
    }
#endif


  public:
    static inline bool countMatchesForTests = false;
    static inline int64_t matchCallsForTests = 0;
    // A/B hook: turn off the per-doc pre-position score bound in doMatches().
    static inline bool disableDocBoundForTests = false;
    // A/B hook: keep phrase execution in query text order instead of docFreq order.
    static inline bool disableSortForTests = false;
    // A/B hook: give every repeated term its own enum, as before dedup.
    static inline bool disableRepeatDedupForTests = false;

    Scorer(MemPool& targetPool, std::span<DocsEnum*> docsEnums, std::span<const int32_t> positions, NormsReader* normsReader,
           Similarity::BM25Scorer* simScorer, std::span<ImpactsIndex> impacts = {},
           std::span<const int32_t> slotGroup = {}, std::span<RepeatGroup> groups = {})
            : docsEnums(docsEnums), positions(positions), pool(&targetPool), slotGroup(slotGroup),
              groups(groups), simScorer(simScorer), impacts(impacts) {
      hasRepeats = !groups.empty();
      if (hasRepeats) {
        slotCursor = targetPool.make_span<int32_t>(docsEnums.size());
      }
      // Scoring needs both BM25 and norms, or neither.
      assert((simScorer == nullptr) == (normsReader == nullptr));
      if (normsReader != nullptr) {
        normsIter.emplace(*normsReader);
        flatNormsBase = normsReader->flatBase();
      }
      int32_t maxOff = 0;
      for (auto pos: positions) {
        maxOff = std::max(maxOff, pos);
      }
      largestPossiblePos = PostingsReader::END - 1 - maxOff;
      for (auto* docsEnum : docsEnums) {
        int32_t numDocs = docsEnum->numDocs();
        if (numDocs > 0) {
          matchCostEstimate += (float) docsEnum->totalTermFreq() / (float) numDocs;
        } else {
          matchCostEstimate += 1.0f;
        }
      }
    }

    int32_t nextApprox() {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doApproximationNext();
    }

    int32_t advanceApprox(int32_t docid) {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doApproximationAdvance(docid);
    }

    bool confirmMatch() {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doMatches();
    }

    bool hasTwoPhase() const override {
      return true;
    }

    int32_t approximationNext() override {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doApproximationNext();
    }

    int32_t approximationAdvance(int32_t target) override {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doApproximationAdvance(target);
    }

    int32_t approximationDocId() override {
      return docid;
    }

    bool matches() override {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doMatches();
    }

    float matchCost() override {
      return matchCostEstimate;
    }

    int32_t next() override {
#ifndef NDEBUG
      markSinglePhase();
#endif
      if (docid == PostingsReader::END) {
        return PostingsReader::END;
      }
      doApproximationNext();
      for (;;) {
        if (docid == PostingsReader::END) {
          return PostingsReader::END;
        }
        if (doMatches()) {
          return docid;
        }
        doApproximationNext();
      }
    }

    int32_t advance(int32_t target) override {
      // confirmMatch() consumes positions, so strict advance must not recheck the
      // current doc.
#ifndef NDEBUG
      markSinglePhase();
#endif
      assert(docid < target);
      doApproximationAdvance(target);
      for (;;) {
        if (docid == PostingsReader::END) {
          return PostingsReader::END;
        }
        if (doMatches()) {
          return docid;
        }
        doApproximationNext();
      }
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docid;
    }

    int32_t numMatches() {
      while (pos != PostingsReader::END) {
        if (!hasRepeats) {
          doNextPosition(docsEnums[0]->nextPosition() - positions[0]);
        } else {
          doNextPositionRepeats(repeatSlotNext(0) - positions[0]);
        }
      }
      return freq;
    }

    void setMinCompetitiveScore(float minScore) override {
      minCompetitiveScore = minScore;
    }

    // Upper bound on the phrase score over [shallow target, upTo] (or from the
    // current doc before any advanceShallow): the min across terms of each
    // term's max block impact over that range (phraseFreq <= every term's freq
    // makes each term's bound valid for the phrase).  Shallow-based, like
    // TermQuery::Scorer::getMaxScore - see the quadratic-hop note there.
    float getMaxScore(int32_t upTo) override {
      if (impacts.empty()) {
        return std::numeric_limits<float>::infinity();
      }
      int32_t startDoc = shallowTarget >= 0 ? shallowTarget : docid;
      float maxScore = std::numeric_limits<float>::infinity();
      for (const auto& termImpacts : impacts) {
        int32_t startBlock = termImpacts.blockContaining(startDoc);
        if (startBlock >= termImpacts.blockCount()) {
          continue;  // no data for the range from this term; it cannot lower the min
        }
        int32_t upBlock = termImpacts.blockContaining(upTo);
        if (upBlock >= termImpacts.blockCount()) {
          upBlock = termImpacts.blockCount() - 1;
        }
        if (upBlock < startBlock) {
          continue;
        }
        float termMax;
        if (upBlock == termImpacts.blockCount() - 1) {
          termMax = termImpacts.maxImpactFrom(startBlock);
        } else {
          termMax = termImpacts.maxImpactInRange(startBlock, upBlock);
        }
        maxScore = std::min(maxScore, termMax);
      }
      return maxScore;
    }

    // The bound above target is stable up to the earliest per-term block end.
    int32_t advanceShallow(int32_t target) override {
      if (impacts.empty()) {
        return PostingsReader::END;
      }
      shallowTarget = target;
      int32_t upTo = PostingsReader::END;
      for (const auto& termImpacts : impacts) {
        int32_t block = termImpacts.blockContaining(target);
        if (block >= termImpacts.blockCount()) {
          return PostingsReader::END;  // a term has no docs past target -> neither does the phrase
        }
        upTo = std::min(upTo, termImpacts.lastDoc(block));
      }
      return upTo;
    }

    float score() override {
      if (simScorer == nullptr) return 0.0f;
      return simScorer->score((float) freq, lookupNorm(docid));
    }
  };


};

} // namespace solux
