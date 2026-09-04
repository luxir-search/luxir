#pragma once

#include "Query.h"

namespace luxir {

// A query that matches no documents. Query builders use this when analysis
// yields zero terms but the caller still needs a valid Query object. The weight
// yields a null scorer for every segment.
class MatchNoDocsQuery final : public luxir::Query {
public:
  MatchNoDocsQuery() : Query(QueryKind::NONE) {}

  bool equalsSameKind(const Query& other) const override {
    unused(other);
    return true;
  }

  uint64_t hashImpl() const override { return Query::hashImpl(); }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  MatchNoDocsQuery::Weight* createWeight(Context& context, int32_t flags,
                                         float multiplier = 1.0f) override {
    unused(multiplier);
    return context.pool.make<MatchNoDocsQuery::Weight>(context, *this, flags);
  }

  class Weight final : public Query::Weight {
  public:
    Weight(Context& context, const MatchNoDocsQuery& query, int32_t flags)
      : Query::Weight(context, query, flags) {
      traits |= IS_CONSTANT_SCORING;  // vacuously constant
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool,
        luxir::IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(targetPool, segment, executionMode);
      return nullptr;
    }

    Query::Scorer* createScorer(luxir::MemPool& targetPool, luxir::IndexReader::Segment& segment) override {
      unused(targetPool);
      unused(segment);
      return nullptr;  // never matches any doc
    }

    std::optional<int64_t> constantCount(
        IndexReader::Segment& segment, DocSet* domain) override {
      unused(segment, domain);
      return 0;
    }
  };
};

} // namespace luxir
