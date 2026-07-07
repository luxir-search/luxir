#pragma once

#include <algorithm>
#include <optional>

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
      auto docsEnums = targetPool.make_span<DocsEnum*>(cachedTermInfos.size());
      for (int i = 0; i < cachedTermInfos.size(); i++) {
        docsEnums[i] = cachedTermInfos[i]->useDocsEnum(targetPool, segment);
        if (docsEnums[i] == nullptr) {
          // term doesn't exist in this segment
          return nullptr;
        }
      }

      // Position matching does not need norms when score() is never read.
      NormsReader* normsReader = (inputFlags & NEED_SCORES) != 0
              ? targetPool.make<NormsReader>(segment.postingsReader(), *segFieldInfo)
              : nullptr;

      // Per-term block impact indexes evaluated with the PHRASE's scorer:
      // phraseFreq <= each term's freq in a doc, so every term's (maxTf,
      // minNorm) frontier bounds the phrase score, and the min across terms
      // bounds any doc range.  Much looser than a term's own bound (real
      // phrase freq is usually far below the min term tf), so pruning bites
      // later - but skipped ranges never touch their position bytes.
      std::span<ImpactsIndex> impacts;
      if (simScorer != nullptr && normsReader != nullptr) {
        auto built = targetPool.make_span<ImpactsIndex>(docsEnums.size());
        bool allBuilt = true;
        for (size_t i = 0; i < docsEnums.size(); i++) {
          built[i].build(targetPool, *docsEnums[i], *simScorer, 1.0f);
          allBuilt &= !built[i].empty();
        }
        if (allBuilt) {
          impacts = built;  // a term without impact data (pulsed) disables bounding
        }
      }

      // no need to make copy, the query will outlive the scorers.
      // auto pos = targetPool.copy_span<const int32_t>(query.getPositions());

      return targetPool.make<PhraseQuery::Scorer>(targetPool, docsEnums, query.getPositions(), normsReader, simScorer, impacts);
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
    std::span<DocsEnum*> docsEnums;
    std::span<const int32_t> positions;
    // Both absent when scores are not needed; score() is 0.
    std::optional<NormsReader::Iterator> normsIter;
    Similarity::BM25Scorer* simScorer;
    // One impact index per term, evaluated with the phrase's simScorer; empty
    // when not scoring or when any term lacks impact data.
    std::span<ImpactsIndex> impacts;
    float minCompetitiveScore = 0.0f;
    // Candidates <= competitiveUpTo passed the bound check under the current
    // threshold; they skip re-evaluation (the common case is many candidates
    // per block range).  Reset when the threshold rises.
    int32_t competitiveUpTo = -1;
    int32_t shallowTarget = -1;
    int64_t skippedRangeCount = 0;

    int32_t docid = -1;
    int32_t pos = -1;    // position of last match, or END if no more matches.
    int32_t freq = 0;
    int32_t largestPossiblePos;   // largest possible position for a match
    int32_t checkedDocid = -1;
    bool checkedMatch = false;
    float matchCostEstimate = 0.0f;
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

    int32_t doApproximationNext() {
      return doNext(docsEnums[0]->next());
    }

    int32_t doApproximationAdvance(int32_t target) {
      return doNext(docsEnums[0]->advance(target));
    }

    // Hop the conjunction past doc-block ranges whose phrase score bound cannot
    // reach the collector's threshold, leaving docid on a candidate worth
    // position-verifying (or END).  Skipped ranges never touch their position
    // bytes: the doc-level skip repairs the position stream lazily.  Only the
    // single-phase drivers (next/advance) prune; matches() stays pure so a
    // two-phase parent gets unchanged match semantics.
    void advanceToCompetitive() {
      if (docid <= competitiveUpTo) {
        return;  // this block range already passed under the current threshold
      }
      if (impacts.empty() || !(minCompetitiveScore > 0.0f) || disablePruningForTests) {
        return;
      }
      while (docid != PostingsReader::END) {
        float bound = std::numeric_limits<float>::infinity();
        float remainingBound = std::numeric_limits<float>::infinity();
        int32_t rangeEnd = PostingsReader::END - 1;
        for (const auto& termImpacts : impacts) {
          int32_t block = termImpacts.blockContaining(docid);
          // a conjunction candidate lies within every term's postings
          assert(block < termImpacts.blockCount());
          bound = std::min(bound, termImpacts.impact(block));
          remainingBound = std::min(remainingBound, termImpacts.maxImpactFrom(block));
          rangeEnd = std::min(rangeEnd, termImpacts.lastDoc(block));
        }
        if (bound >= minCompetitiveScore) {
          competitiveUpTo = rangeEnd;
          return;
        }
        skippedRangeCount++;
        if (remainingBound < minCompetitiveScore) {
          // no later doc can compete for ANY term; the phrase is done
          docid = PostingsReader::END;
          return;
        }
        if (rangeEnd >= PostingsReader::END - 1) {
          docid = PostingsReader::END;
          return;
        }
        doApproximationAdvance(rangeEnd + 1);
      }
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
      for (auto* docsEnum: docsEnums) {
        docsEnum->startPositions();
      }
      checkedMatch = doNextPosition(docsEnums[0]->advancePosition(positions[0]) - positions[0]) != PostingsReader::END;
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
    // A/B hook: turn off impact-based range skipping (bounds still built).
    static inline bool disablePruningForTests = false;

    Scorer(MemPool& targetPool, std::span<DocsEnum*> docsEnums, std::span<const int32_t> positions, NormsReader* normsReader,
           Similarity::BM25Scorer* simScorer, std::span<ImpactsIndex> impacts = {})
            : docsEnums(docsEnums), positions(positions), simScorer(simScorer), impacts(impacts) {
      unused(targetPool);
      // Scoring needs both BM25 and norms, or neither.
      assert((simScorer == nullptr) == (normsReader == nullptr));
      if (normsReader != nullptr) normsIter.emplace(*normsReader);
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
        advanceToCompetitive();
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
        advanceToCompetitive();
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
        doNextPosition(docsEnums[0]->nextPosition() - positions[0]);
      }
      return freq;
    }

    void setMinCompetitiveScore(float minScore) override {
      minCompetitiveScore = minScore;
      competitiveUpTo = -1;  // re-evaluate ranges under the higher threshold
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

    int64_t skippedRanges() const {
      return skippedRangeCount;
    }

    float score() override {
      if (simScorer == nullptr) return 0.0f;
      int32_t normDoc = normsIter->advance(docid);
      assert(normDoc == docid);
      auto encodedNorm = normsIter->value();
      return simScorer->score((float) freq, encodedNorm);
    }
  };


};

} // namespace solux
