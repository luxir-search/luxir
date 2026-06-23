#pragma once

#include <optional>

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
        return targetPool.make<TermQuery::Scorer>(*docsEnum, nullptr, nullptr);
      }

      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord]; // this segFieldInfo can't be null at this point
      solux::IntColReader* normsReader = targetPool.make<solux::IntColReader>(segment.postingsReader(),
                                                                              *segFieldInfo);
      return targetPool.make<TermQuery::Scorer>(*docsEnum, normsReader, cachedTermInfo->simScorer);
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

    Scorer(solux::DocsEnum& docsEnum, solux::IntColReader* normsReader, solux::Similarity::BM25Scorer* simScorer)
            : docsEnum(docsEnum), simScorer(simScorer) {
      // Scoring needs both BM25 and norms, or neither.
      assert((simScorer == nullptr) == (normsReader == nullptr));
      if (normsReader != nullptr) normsIter.emplace(*normsReader);
    }

    int32_t next() override {
      return docsEnum.nextDoc();
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
      return simScorer->score((float) tf, encodedNorm);
    }

    /// term frequency for current doc
    int termFreq() {
      return docsEnum.termFreq();
    }

    // make a pusher / visitor for term scorer?

  };

};

} // namespace solux
