#pragma once

#include "Query.h"

namespace solux {

// A query that matches no documents. The query builder returns this when
// query-time analysis yields zero terms (an all-punctuation phrase, empty
// input, etc.): there is nothing to match, but we still need a valid Query to
// hand back. It mirrors the "no match" path TermQuery already takes for a
// missing term/field -- the weight simply yields a null scorer for every
// segment, which every caller already treats as "this source can't match here".
class MatchNoDocsQuery final : public solux::Query {
public:
  MatchNoDocsQuery() {}

  MatchNoDocsQuery::Weight* createWeight(Context& context) override {
    return context.pool.make<MatchNoDocsQuery::Weight>(context);
  }

  class Weight final : public Query::Weight {
  public:
    explicit Weight(Context& context) : Query::Weight(context) {}

    Query::Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      unused(targetPool);
      unused(segment);
      return nullptr;  // never matches any doc
    }
  };
};

} // namespace solux
