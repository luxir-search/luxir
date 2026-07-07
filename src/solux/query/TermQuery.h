#pragma once

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "Query.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/NormsReader.h"
#include "solux/util/solux_util.h"

namespace solux {

class TermQuery final : public solux::Query {
protected:
  std::string_view field;
  std::string_view term;
  Similarity::TermStats injectedTermStats = {};
  float boost;
  bool useFrontierBound;
  bool hasInjectedTermStats = false;
public:
  TermQuery(std::string_view field, std::string_view term, float boost = 1.0f,
            bool useFrontierBound = true)
      : field(field), term(term), boost(boost), useFrontierBound(useFrontierBound) {}

  TermQuery(std::string_view field, std::string_view term,
            const Similarity::TermStats& injectedTermStats, float boost = 1.0f,
            bool useFrontierBound = true)
      : field(field), term(term), injectedTermStats(injectedTermStats), boost(boost),
        useFrontierBound(useFrontierBound), hasInjectedTermStats(true) {}

  std::string_view getField() const {
    return field;
  }

  std::string_view getTerm() const {
    return term;
  }

  float getBoost() const {
    return boost;
  }

  bool shouldUseFrontierBound() const {
    return useFrontierBound;
  }

  bool hasInjectedStats() const {
    return hasInjectedTermStats;
  }

  const Similarity::TermStats& scoringTermStats(const CachedTermInfo& cachedTermInfo) const {
    return hasInjectedTermStats ? injectedTermStats : cachedTermInfo.termStats;
  }

  TermQuery::Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<TermQuery::Weight>(context, *this, flags);
  }

  class Weight final : public Query::Weight {
  protected:
    TermQuery& query;
    solux::CachedFieldInfo* cachedFieldInfo = nullptr;
    solux::CachedTermInfo* cachedTermInfo = nullptr;
    solux::Similarity::BM25Scorer* simScorer = nullptr;
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
      // read. FuzzyQuery injects blended stats per clause, so those scorers are
      // weight-local; normal term queries keep sharing the cached scorer.
      if (needScores && cachedTermInfo != nullptr) {
        if (query.hasInjectedStats()) {
          simScorer = context.pool.make<solux::Similarity::BM25Scorer>(
              solux::Similarity().getScorer(
                  1.0f, cachedFieldInfo->fieldStats, query.scoringTermStats(*cachedTermInfo)));
        } else {
          if (cachedTermInfo->simScorer == nullptr) {
            cachedTermInfo->simScorer = context.pool.make<solux::Similarity::BM25Scorer>(
                solux::Similarity().getScorer(1.0f, cachedFieldInfo->fieldStats,
                                              cachedTermInfo->termStats));
          }
          simScorer = cachedTermInfo->simScorer;
        }
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
      solux::NormsReader* normsReader = nullptr;
      solux::IntColReader* valueReader = nullptr;
      if (segFieldInfo->type == FieldType::TEXT) {
        normsReader = targetPool.make<solux::NormsReader>(segment.postingsReader(), *segFieldInfo);
      } else if (segFieldInfo->columnLoc.offset() > 0) {
        valueReader = targetPool.make<solux::IntColReader>(segment.postingsReader(), *segFieldInfo);
      }
      return targetPool.make<TermQuery::Scorer>(targetPool, *docsEnum, normsReader, valueReader,
                                                simScorer, query.getBoost(),
                                                query.shouldUseFrontierBound());
    }

    // A term's exact match count is its docFreq - free from the term stats -
    // unless deletions could have removed some of its docs.
    int64_t count(solux::IndexReader::Segment& segment) override {
      if (segment.liveDocs() != nullptr) {
        return -1;
      }
      if (cachedTermInfo == nullptr) {
        return 0;
      }
      auto* docsEnum = cachedTermInfo->docsEnums[segment.ord];
      return docsEnum == nullptr ? 0 : docsEnum->numDocs();
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
    // Both absent when scores are not needed; score() is 0. Text fields use
    // normsIter; non-text term queries keep the existing column-backed lookup.
    std::optional<solux::NormsReader::Iterator> normsIter;
    std::optional<solux::IntColReader::Iterator> valueIter;
    const uint8_t* flatNormsBase = nullptr;
    solux::Similarity::BM25Scorer* simScorer;
    int32_t impactBlockCount = 0;
    int32_t* impactLastDoc = nullptr;
    float* blockImpact = nullptr;
    float* maxImpactFrom = nullptr;
    float minCompetitiveScore = 0.0f;
    int32_t shallowBlock = -1;
    // Query-time multiplier for boosted term clauses, e.g. fuzzy rewrites.
    float boost;
    int64_t skippedImpactBlocks = 0;

    Scorer(solux::DocsEnum& docsEnum, solux::NormsReader* normsReader,
           solux::Similarity::BM25Scorer* simScorer, float boost = 1.0f,
           bool useFrontierBound = true)
            : Scorer(docsEnum, normsReader, nullptr, simScorer, boost, useFrontierBound) {
    }

    Scorer(solux::DocsEnum& docsEnum, solux::NormsReader* normsReader,
           solux::IntColReader* valueReader, solux::Similarity::BM25Scorer* simScorer,
           float boost = 1.0f, bool useFrontierBound = true)
            : docsEnum(docsEnum), simScorer(simScorer), boost(boost) {
      unused(useFrontierBound);
      // Scoring needs both BM25 and an encoded norm/value lookup, or neither.
      assert((simScorer == nullptr) == (normsReader == nullptr && valueReader == nullptr));
      assert(normsReader == nullptr || valueReader == nullptr);
      if (normsReader != nullptr) {
        normsIter.emplace(*normsReader);
        flatNormsBase = normsReader->flatBase();
      }
      if (valueReader != nullptr) valueIter.emplace(*valueReader);
    }

    Scorer(solux::MemPool& pool, solux::DocsEnum& docsEnum, solux::NormsReader* normsReader,
           solux::Similarity::BM25Scorer* simScorer, float boost = 1.0f,
           bool useFrontierBound = true)
            : Scorer(docsEnum, normsReader, simScorer, boost, useFrontierBound) {
      buildImpacts(pool, normsReader != nullptr, useFrontierBound);
    }

    Scorer(solux::MemPool& pool, solux::DocsEnum& docsEnum, solux::NormsReader* normsReader,
           solux::IntColReader* valueReader, solux::Similarity::BM25Scorer* simScorer,
           float boost = 1.0f, bool useFrontierBound = true)
            : Scorer(docsEnum, normsReader, valueReader, simScorer, boost, useFrontierBound) {
      buildImpacts(pool, normsReader != nullptr || valueReader != nullptr, useFrontierBound);
    }

    bool hasImpacts() const {
      return impactBlockCount > 0;
    }

    void buildImpacts(solux::MemPool& pool, bool hasNormLookup,
                      bool useFrontierBound = true) {
      if (simScorer == nullptr || !hasNormLookup) {
        return;
      }

      // T2 frontier bound: evaluate each block's impact at all real Pareto frontier
      // points.  The old T1 corner bound remains available for A/B measurement.
      std::vector<int32_t> blockMaxTf;
      std::vector<int32_t> blockLastDoc;
      std::vector<int32_t> blockMinNorm;
      DocsEnum::ImpactFrontiers impactFrontiers;
      docsEnum.readBlockMaxTf(blockMaxTf, nullptr, &blockLastDoc, &blockMinNorm,
                              nullptr, useFrontierBound ? &impactFrontiers : nullptr);
      if (blockMaxTf.empty()) {
        return;
      }
      assert(blockMaxTf.size() == blockLastDoc.size());
      assert(blockMaxTf.size() == blockMinNorm.size());
      assert(!useFrontierBound
             || impactFrontiers.offsets.size() == blockMaxTf.size() + 1);

      impactBlockCount = (int32_t) blockMaxTf.size();
      impactLastDoc = pool.make_arr<int32_t>((size_t) impactBlockCount);
      blockImpact = pool.make_arr<float>((size_t) impactBlockCount);
      maxImpactFrom = pool.make_arr<float>((size_t) impactBlockCount);

      for (int32_t i = 0; i < impactBlockCount; i++) {
        impactLastDoc[i] = blockLastDoc[i];
        if (useFrontierBound) {
          int32_t start = impactFrontiers.offsets[(size_t) i];
          int32_t end = impactFrontiers.offsets[(size_t) i + 1];
          if (start < end) {
            float maxImpact = 0.0f;
            for (int32_t j = start; j < end; j++) {
              maxImpact = std::max(
                  maxImpact,
                  boost * simScorer->score((float) impactFrontiers.tfs[(size_t) j],
                                            (int64_t) impactFrontiers.norms[(size_t) j]));
            }
            blockImpact[i] = maxImpact;
          } else {
            blockImpact[i] = boost * simScorer->score((float) blockMaxTf[i], (int64_t) blockMinNorm[i]);
          }
        } else {
          blockImpact[i] = boost * simScorer->score((float) blockMaxTf[i], (int64_t) blockMinNorm[i]);
        }
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
          skippedImpactBlocks += impactBlockCount - block;
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
        skippedImpactBlocks++;
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

    // Keep the norms advance out-of-line at the per-doc score() call site.
    //
    // score() is the vtable target the per-doc disjunction path calls once per
    // matching doc. Keep norm lookup out-of-line here so scorer code size stays
    // stable. Block scoring (fillScoresFromSpans) deliberately does NOT use this
    // helper, so the block/throughput path still inlines the lookup.
    //
    // Without this NOINLINE, BM_FullTextScoreTopKBulkDisjunction/bulk_few
    // regressed +43.5% in a gcc-release A/B - and bulk_few does not even execute
    // the edit that triggered it; the inliner just re-evaluated the whole TU.
    // This is a fragile codegen workaround, not a fundamental constraint.
    // REVISIT with a future compiler: re-run that benchmark with and without
    // SOLUX_NOINLINE and drop it if the inliner no longer over-pulls.
    // Observed on: g++ (Ubuntu) 16.0.1 20260322 experimental (trunk r16-8246).
    int64_t lookupNorm(int32_t doc) {
      // Flat norms (the common text case) are a direct byte load - same
      // equivalence the block path (fillScoresFromSpans) already relies on.
      if (flatNormsBase != nullptr) {
        return flatNormsBase[doc];
      }
      if (normsIter) {
        int32_t normDoc = normsIter->advance(doc);
        assert(normDoc == doc);
        return normsIter->value();
      }
      int32_t normDoc = valueIter->advance(doc);
      assert(normDoc == doc);
      return valueIter->value();
    }

    int64_t SOLUX_NOINLINE advanceNorm(int32_t doc) {
      return lookupNorm(doc);
    }

    float score() override {
      if (simScorer == nullptr) return 0.0f;
      auto docid = docsEnum.docId();
      int32_t tf = docsEnum.termFreq();
      // Keep the flat-norms load inline (it is one indexed byte read); the
      // NOINLINE advanceNorm wrapper stays for the sparse iterator walk only,
      // which is the code the inliner used to over-pull into score().
      int64_t encodedNorm = flatNormsBase != nullptr ? flatNormsBase[docid]
                                                     : advanceNorm(docid);
      return boost * simScorer->score((float) tf, encodedNorm);
    }

    int32_t fillScoreBlockScalar(int32_t* docs, float* scores, int32_t count, int32_t upTo,
                                 bool includeCurrent) {
      assert(count >= 0);
      int32_t filled = 0;
      int32_t doc = docsEnum.docId();
      if (doc < 0) {
        doc = skipNonCompetitiveBlocks(docsEnum.nextDoc());
      } else if (!includeCurrent) {
        if (doc >= PostingsReader::END - 1) {
          return 0;
        }
        doc = skipNonCompetitiveBlocks(docsEnum.advance(doc + 1));
      }
      while (filled < count && doc < upTo) {
        docs[filled] = doc;
        if (simScorer == nullptr) {
          scores[filled] = 0.0f;
        } else {
          int32_t tf = docsEnum.termFreq();
          auto encodedNorm = lookupNorm(doc);
          scores[filled] = boost * simScorer->score((float) tf, encodedNorm);
        }
        filled++;
        doc = skipNonCompetitiveBlocks(docsEnum.nextDoc());
      }
      return filled;
    }

    void fillScoresFromSpans(int32_t* docs, float* scores, std::span<const int32_t> blockDocs,
                             std::span<const int32_t> blockFreqs, int32_t count) {
      for (int32_t i = 0; i < count; i++) {
        docs[i] = blockDocs[(size_t) i];
      }
      if (simScorer == nullptr) {
        std::fill(scores, scores + count, 0.0f);
        return;
      }
      if (flatNormsBase != nullptr) {
        assert(count <= Postings::DOCS_BLOCK_SIZE);
        uint8_t normBuf[Postings::DOCS_BLOCK_SIZE];
        for (int32_t i = 0; i < count; i++) {
          int32_t doc = blockDocs[(size_t) i];
          normBuf[i] = flatNormsBase[doc];
        }
        simScorer->scoreBlock(blockFreqs.data(), normBuf, boost, scores, count);
        return;
      }
      for (int32_t i = 0; i < count; i++) {
        int32_t doc = blockDocs[(size_t) i];
        auto encodedNorm = lookupNorm(doc);
        scores[i] = boost * simScorer->score((float) blockFreqs[(size_t) i], encodedNorm);
      }
    }

    int32_t fillScoreBlock(int32_t* docs, float* scores, int32_t count, int32_t upTo) override {
      assert(count >= 0);
      if (count <= 0) {
        return 0;
      }

      // The scalar path owns exact impact-threshold skipping. The block span
      // path is used by BS1, which does not push child term thresholds.
      if (hasImpacts() && minCompetitiveScore > 0.0f) {
        return fillScoreBlockScalar(docs, scores, count, upTo, true);
      }

      int32_t filled = 0;
      while (filled < count) {
        auto [blockDocs, blockFreqs] = docsEnum.peekDocFreqBlock();
        int32_t available = (int32_t) blockDocs.size();
        if (available == 0) {
          break;
        }

        int32_t limit = std::min(available, count - filled);
        int32_t used = 0;
        if (blockDocs[(size_t) limit - 1] < upTo) {
          used = limit;
        } else {
          while (used < limit && blockDocs[(size_t) used] < upTo) {
            used++;
          }
        }

        if (used == 0) {
          break;
        }
        fillScoresFromSpans(docs + filled, scores + filled, blockDocs, blockFreqs, used);
        docsEnum.consumeDocFreqBlock(used);
        filled += used;

        if (used < available) {
          break;
        }
      }
      return filled;
    }

    void setMinCompetitiveScore(float minScore) override {
      minCompetitiveScore = minScore;
    }

    float getMaxScore(int32_t upTo) override {
      if (!hasImpacts()) {
        return std::numeric_limits<float>::infinity();
      }

      int32_t docBlock = blockContaining(docsEnum.docId());
      int32_t startBlock = shallowBlock >= 0 ? std::min(docBlock, shallowBlock) : docBlock;
      if (startBlock >= impactBlockCount) {
        return std::numeric_limits<float>::infinity();
      }

      int32_t upBlock = blockContaining(upTo);
      if (upBlock >= impactBlockCount) {
        upBlock = impactBlockCount - 1;
      }
      if (upBlock < startBlock) {
        return std::numeric_limits<float>::infinity();
      }
      if (upBlock == impactBlockCount - 1) {
        return maxImpactFrom[startBlock];
      }

      float maxScore = 0.0f;
      for (int32_t i = startBlock; i <= upBlock; i++) {
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

    int64_t skippedBlocks() const {
      return skippedImpactBlocks;
    }

    /// term frequency for current doc
    int termFreq() {
      return docsEnum.termFreq();
    }

    // make a pusher / visitor for term scorer?

  };

};

} // namespace solux
