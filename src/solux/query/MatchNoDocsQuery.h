#pragma once

#include "Query.h"

namespace solux {

// A query that matches no documents. Query builders use this when analysis
// yields zero terms but the caller still needs a valid Query object. The weight
// yields a null scorer for every segment.
class MatchNoDocsQuery final : public solux::Query {
public:
  MatchNoDocsQuery() {}

  MatchNoDocsQuery::Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<MatchNoDocsQuery::Weight>(context, flags);
  }

  class Weight final : public Query::Weight {
  public:
    Weight(Context& context, int32_t flags) : Query::Weight(context, flags) {
      traits |= IS_CONSTANT_SCORING;  // vacuously constant
    }

    Query::Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      unused(targetPool);
      unused(segment);
      return nullptr;  // never matches any doc
    }
  };
};

} // namespace solux
