#pragma once

#include "Query.h"
#include "solux/reader/IntColReader.h"

namespace solux {

class TermQuery final : public solux::Query {
protected:
  std::string_view field;
  std::string_view term;
  float boost;
public:
  // TermQuery constructor
  TermQuery(std::string_view field, std::string_view term, float boost = 1.0f) : field(field), term(term),
                                                                                 boost(boost) {}

  std::string_view getField() const {
    return field;
  }

  std::string_view getTerm() const {
    return term;
  }

  TermQuery::Weight* createWeight(Context& context) override {
    TermQuery::Weight* weight = context.pool.make<TermQuery::Weight>(context, *this);
    return weight;
  }

  class Weight final : public Query::Weight {
  protected:
    TermQuery& query;
    solux::CachedFieldInfo* cachedFieldInfo = nullptr;
    solux::CachedTermInfo* cachedTermInfo = nullptr;
  public:
    Weight(Context& context, TermQuery& query) : Query::Weight(context), query(query) {
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
      if (cachedFieldInfo != nullptr) {
        cachedTermInfo = context.getCachedTerminfo(*cachedFieldInfo, query.getTerm());
      }
      if (cachedTermInfo != nullptr) {
        if (cachedTermInfo->simScorer == nullptr) {
          cachedTermInfo->simScorer = context.pool.make<solux::Similarity::BM25Scorer>(
                  solux::Similarity().getScorer(1.0f, cachedFieldInfo->fieldStats, cachedTermInfo->termStats));
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


      /* code before caching...
      TermsEnum termsEnum(context.pool, segment.postingsReader(), *segFieldInfo);
      if (!termsEnum.seek(query.getTerm())) {
        return nullptr;
      }
      // If we create a new DocsEnum each time, then we can put it in the targetPool.  If we cache it, it should be
      // cached elsewhere (like the context pool?)
      DocsEnum* docsEnum = targetPool.make<DocsEnum>(targetPool, segment.postingsReader(), termsEnum);
       */

      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord]; // this segFieldInfo can't be null at this point
      solux::IntColReader* normsReader = targetPool.make<solux::IntColReader>(targetPool, segment.postingsReader(),
                                                                              *segFieldInfo);
      return targetPool.make<TermQuery::Scorer>(*docsEnum, *normsReader, *cachedTermInfo->simScorer);
    }

  };  // TermQuery::Weight

  class Scorer final : public Query::Scorer {
  public:
    solux::DocsEnum& docsEnum;
    // IntColReader normsReader; // prob not necessary?
    solux::IntColReader::Iterator normsIter;
    solux::Similarity::BM25Scorer& simScorer; // todo: pass this in, it could be shared
    // point back to weight?  Require TermWeight? Or what if we want to use this from other types of queries though?
    // pass in the ord of this segment? or the actual IndexReader::Segment& seg;

    Scorer(solux::DocsEnum& docsEnum, solux::IntColReader& normsReader, solux::Similarity::BM25Scorer& simScorer)
            : docsEnum(docsEnum), normsIter(normsReader), simScorer(simScorer) {
    }

    int32_t next() override {
      return docsEnum.nextDoc();
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docsEnum.docId();
    }

    float score() override {
      auto docid = docsEnum.docId();
      int32_t tf = docsEnum.termFreq();
      int32_t normDoc = normsIter.advance(docid);
      assert(normDoc == docid);
      auto encodedNorm = normsIter.value();
      return simScorer.score((float) tf, encodedNorm);
    }

    /// term frequency for current doc
    int termFreq() {
      return docsEnum.termFreq();
    }

    // make a pusher / visitor for term scorer?

  }; // TermQuery::Scorer

};

} // namespace solux
