#pragma once

#include <optional>

#include "Query.h"
#include "solux/reader/IntColReader.h"

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
      IntColReader* normsReader = (inputFlags & NEED_SCORES) != 0
              ? targetPool.make<IntColReader>(segment.postingsReader(), *segFieldInfo)
              : nullptr;

      // no need to make copy, the query will outlive the scorers.
      // auto pos = targetPool.copy_span<const int32_t>(query.getPositions());

      return targetPool.make<PhraseQuery::Scorer>(targetPool, docsEnums, query.getPositions(), normsReader, simScorer);
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
    std::optional<IntColReader::Iterator> normsIter;
    Similarity::BM25Scorer* simScorer;

    int32_t docid = -1;
    int32_t pos = -1;    // position of last match, or END if no more matches.
    int32_t freq = 0;
    int32_t largestPossiblePos;   // largest possible position for a match


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


  public:
    Scorer(MemPool& targetPool, std::span<DocsEnum*> docsEnums, std::span<const int32_t> positions, IntColReader* normsReader,
           Similarity::BM25Scorer* simScorer)
            : docsEnums(docsEnums), positions(positions), simScorer(simScorer) {
      // Scoring needs both BM25 and norms, or neither.
      assert((simScorer == nullptr) == (normsReader == nullptr));
      if (normsReader != nullptr) normsIter.emplace(*normsReader);
      int32_t maxOff = 0;
      for (auto pos: positions) {
        maxOff = std::max(maxOff, pos);
      }
      largestPossiblePos = PostingsReader::END - 1 - maxOff;
    }

    int32_t nextApprox() {
      return doNext(docsEnums[0]->next());
    }

    int32_t advanceApprox(int32_t docid) {
      return doNext(docsEnums[0]->advance(docid));
    }

    bool confirmMatch() {
      freq = 0;
      for (auto* docsEnum: docsEnums) {
        docsEnum->startPositions();
      }
      return doNextPosition(docsEnums[0]->advancePosition(positions[0]) - positions[0]) != PostingsReader::END;
    }

    int32_t next() override {
      while (docid < PostingsReader::END) {
        nextApprox();
        if (confirmMatch()) {
          return docid;
        }
      }
      return PostingsReader::END;
    }

    int32_t advance(int32_t target) override {
      // confirmMatch() consumes positions, so strict advance must not recheck the
      // current doc.
      assert(docid < target);
      advanceApprox(target);
      for (;;) {
        if (docid == PostingsReader::END) {
          return PostingsReader::END;
        }
        if (confirmMatch()) {
          return docid;
        }
        nextApprox();
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
