#pragma once

#include "Query.h"

namespace solux {

class AllQuery final : public solux::Query {
public:
  // TermQuery constructor
  AllQuery() {}

  AllQuery::Weight* createWeight(Context& context) override {
    AllQuery::Weight* weight = context.pool.make<AllQuery::Weight>(context, *this);
    return weight;
  }

  class Weight final : public Query::Weight {
  protected:
    AllQuery& query;
  public:
    Weight(Context& context, AllQuery& query) : Query::Weight(context), query(query) {
    }

    AllQuery::Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      return targetPool.make<AllQuery::Scorer>(segment);
    }

  };  // TermQuery::Weight

  class Scorer final : public Query::Scorer {
  public:
    solux::IndexReader::Segment& segment;
    int32_t docid = -1;
    int32_t lastDoc;

    Scorer(solux::IndexReader::Segment& segment) : segment(segment), lastDoc(segment.postingsReader().numDocs() - 1) {
    }

    int32_t next() override {
      if (docid >= lastDoc) {
        docid = PostingsReader::END;
      } else {
        docid++;
      }
      return docid;
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docid;
    }

    float score() override {
      return 0.0f;
    }

  }; // TermQuery::Scorer

};

} // namespace solux
