#pragma once

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

#include "Query.h"
#include "solux/reader/IntColReader.h"

namespace solux {

class TermQuery final : public solux::Query {
protected:
  std::string_view field;
  std::string_view term;
  float boost;
public:
  TermQuery(std::string_view field, std::string_view term, float boost = 1.0f) : field(field), term(term),
                                                                                 boost(boost) {}

  std::string_view getField() const {
    return field;
  }

  std::string_view getTerm() const {
    return term;
  }

  float getBoost() const {
    return boost;
  }

  TermQuery::Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<TermQuery::Weight>(context, *this, flags);
  }

  class Weight final : public Query::Weight {
  protected:
    TermQuery& query;
    solux::CachedFieldInfo* cachedFieldInfo = nullptr;
    solux::CachedTermInfo* cachedTermInfo = nullptr;
  public:
    Weight(Context& context, TermQuery& query, int32_t flags)
            : Query::Weight(context, flags), query(query) {
      bool needScores = (flags & NEED_SCORES) != 0;
      // Filter-style terms match normally but always score 0.
      if (!needScores) traits |= IS_CONSTANT_SCORING;
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
      if (cachedFieldInfo != nullptr) {
        cachedTermInfo = context.getCachedTerminfo(*cachedFieldInfo, query.getTerm());
      }
      // Only set up the BM25 sim scorer when this clause's score is actually
      // read (the cache is shared, so a scoring clause for the same term still
      // creates it lazily).
      if (needScores && cachedTermInfo != nullptr && cachedTermInfo->simScorer == nullptr) {
        cachedTermInfo->simScorer = context.pool.make<solux::Similarity::BM25Scorer>(
                solux::Similarity().getScorer(1.0f, cachedFieldInfo->fieldStats, cachedTermInfo->termStats));
      }
    }


    TermQuery::Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      if (cachedTermInfo == nullptr) {
        // term doesn't exist in any segment
        return nullptr;
      }
      solux::DocsEnum* docsEnum = cachedTermInfo->useDocsEnum(targetPool, segment);
      if (docsEnum == nullptr) {
        // term doesn't exist in this segment
        return nullptr;
      }

      if ((inputFlags & NEED_SCORES) == 0) {
        // Matching does not need norms or BM25 when score() is never read.
        return targetPool.make<TermQuery::Scorer>(*docsEnum, nullptr, nullptr, query.getBoost());
      }

      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord]; // this segFieldInfo can't be null at this point
      solux::IntColReader* normsReader = targetPool.make<solux::IntColReader>(segment.postingsReader(),
                                                                              *segFieldInfo);
      return targetPool.make<TermQuery::Scorer>(targetPool, *docsEnum, normsReader,
                                                cachedTermInfo->simScorer, query.getBoost());
    }

    // Per-segment supplier that exposes the term's real cost (its number of docs
    // in this segment) so compound scorers can order leaders by cost. The
    // default supplier reports maxDoc for every clause, which is useless for
    // e.g. the min-should-match lead/tail split.
    class Supplier final : public Query::ScorerSupplier {
      TermQuery::Weight& weight;
      solux::IndexReader::Segment& segment;
    public:
      Supplier(TermQuery::Weight& weight, solux::IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override {
        if (weight.cachedTermInfo == nullptr) return 0;
        auto* docsEnum = weight.cachedTermInfo->docsEnums[segment.ord];
        return docsEnum == nullptr ? 0 : docsEnum->numDocs();
      }

      Query::Scorer* get(solux::MemPool& targetPool, int64_t leadCost) override {
        unused(leadCost);
        return weight.createScorer(targetPool, segment);
      }
    };

    Query::ScorerSupplier* scorerSupplier(solux::MemPool& targetPool,
                                          solux::IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment);
    }

  };

  class Scorer final : public Query::Scorer {
  public:
    solux::DocsEnum& docsEnum;
    // Both absent when scores are not needed; score() is 0.
    std::optional<solux::IntColReader::Iterator> normsIter;
    solux::Similarity::BM25Scorer* simScorer;
    int32_t impactBlockCount = 0;
    int32_t* impactLastDoc = nullptr;
    float* blockImpact = nullptr;
    float* maxImpactFrom = nullptr;
    float minCompetitiveScore = 0.0f;
    int32_t shallowBlock = -1;
    // Query-time multiplier for boosted term clauses, e.g. fuzzy rewrites.
    float boost;

    Scorer(solux::DocsEnum& docsEnum, solux::IntColReader* normsReader,
           solux::Similarity::BM25Scorer* simScorer, float boost = 1.0f)
            : docsEnum(docsEnum), simScorer(simScorer), boost(boost) {
      // Scoring needs both BM25 and norms, or neither.
      assert((simScorer == nullptr) == (normsReader == nullptr));
      if (normsReader != nullptr) normsIter.emplace(*normsReader);
    }

    Scorer(solux::MemPool& pool, solux::DocsEnum& docsEnum, solux::IntColReader* normsReader,
           solux::Similarity::BM25Scorer* simScorer, float boost = 1.0f)
            : Scorer(docsEnum, normsReader, simScorer, boost) {
      buildImpacts(pool, normsReader);
    }

    bool hasImpacts() const {
      return impactBlockCount > 0;
    }

    void buildImpacts(solux::MemPool& pool, solux::IntColReader* normsReader) {
      if (simScorer == nullptr || normsReader == nullptr) {
        return;
      }
      int64_t minNorm = normsReader->getMin();
      if (minNorm < 0 || minNorm > 255) {
        return;
      }

      std::vector<int32_t> blockMaxTf;
      std::vector<int32_t> blockLastDoc;
      docsEnum.readBlockMaxTf(blockMaxTf, nullptr, &blockLastDoc);
      if (blockMaxTf.empty()) {
        return;
      }
      assert(blockMaxTf.size() == blockLastDoc.size());

      impactBlockCount = (int32_t) blockMaxTf.size();
      impactLastDoc = pool.make_arr<int32_t>((size_t) impactBlockCount);
      blockImpact = pool.make_arr<float>((size_t) impactBlockCount);
      maxImpactFrom = pool.make_arr<float>((size_t) impactBlockCount);

      for (int32_t i = 0; i < impactBlockCount; i++) {
        impactLastDoc[i] = blockLastDoc[i];
        blockImpact[i] = boost * simScorer->score((float) blockMaxTf[i], minNorm);
      }
      float suffixMax = 0.0f;
      for (int32_t i = impactBlockCount - 1; i >= 0; i--) {
        suffixMax = std::max(suffixMax, blockImpact[i]);
        maxImpactFrom[i] = suffixMax;
      }
    }

    int32_t blockContaining(int32_t target) const {
      if (!hasImpacts()) {
        return impactBlockCount;
      }
      int32_t* begin = impactLastDoc;
      int32_t* end = impactLastDoc + impactBlockCount;
      int32_t* it = std::lower_bound(begin, end, target);
      return (int32_t) (it - begin);
    }

    int32_t skipNonCompetitiveBlocks(int32_t doc) {
      if (!hasImpacts() || !(minCompetitiveScore > 0.0f)) {
        return doc;
      }
      while (doc != PostingsReader::END) {
        int32_t block = blockContaining(doc);
        if (block >= impactBlockCount) {
          return doc;
        }
        if (maxImpactFrom[block] < minCompetitiveScore) {
          return PostingsReader::END;
        }
        if (blockImpact[block] >= minCompetitiveScore) {
          return doc;
        }
        if (impactLastDoc[block] >= PostingsReader::END - 1) {
          return PostingsReader::END;
        }
        int32_t target = impactLastDoc[block] + 1;
        if (target <= doc) {
          return doc;
        }
        doc = docsEnum.advance(target);
      }
      return doc;
    }

    int32_t next() override {
      return skipNonCompetitiveBlocks(docsEnum.nextDoc());
    }

    int32_t advance(int32_t target) override {
      return docsEnum.advance(target);
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docsEnum.docId();
    }

    float score() override {
      if (simScorer == nullptr) return 0.0f;
      auto docid = docsEnum.docId();
      int32_t tf = docsEnum.termFreq();
      int32_t normDoc = normsIter->advance(docid);
      assert(normDoc == docid);
      auto encodedNorm = normsIter->value();
      return boost * simScorer->score((float) tf, encodedNorm);
    }

    void setMinCompetitiveScore(float minScore) override {
      minCompetitiveScore = minScore;
    }

    float getMaxScore(int32_t upTo) override {
      if (!hasImpacts()) {
        return std::numeric_limits<float>::infinity();
      }

      int32_t curBlock = blockContaining(docsEnum.docId());
      if (curBlock >= impactBlockCount) {
        return std::numeric_limits<float>::infinity();
      }

      int32_t upBlock = blockContaining(upTo);
      if (upBlock >= impactBlockCount) {
        upBlock = impactBlockCount - 1;
      }
      if (upBlock < curBlock) {
        return std::numeric_limits<float>::infinity();
      }
      if (upBlock == impactBlockCount - 1) {
        return maxImpactFrom[curBlock];
      }

      float maxScore = 0.0f;
      for (int32_t i = curBlock; i <= upBlock; i++) {
        maxScore = std::max(maxScore, blockImpact[i]);
      }
      return maxScore;
    }

    int32_t advanceShallow(int32_t target) override {
      if (!hasImpacts()) {
        return PostingsReader::END;
      }
      shallowBlock = blockContaining(target);
      if (shallowBlock >= impactBlockCount) {
        return PostingsReader::END;
      }
      return impactLastDoc[shallowBlock];
    }

    /// term frequency for current doc
    int termFreq() {
      return docsEnum.termFreq();
    }

    // make a pusher / visitor for term scorer?

  };

};

} // namespace solux
