#pragma once

#include <utility>

#include "Query.h"
#include "QueryPrep.h"

namespace solux {

// Debug/testing wrapper that forces a child query through prepare() without
// changing its matches or scores.
class ForcePrepareQuery final : public solux::Query {
  Query* child;

public:
  explicit ForcePrepareQuery(Query* child) : child(child) {}

  Weight* createWeight(Context& context) override {
    return context.pool.make<ForcePrepareQuery::Weight>(context, *this);
  }

  class Weight final : public Query::Weight {
    Query::Weight* childWeight = nullptr;

    class Prepared final : public Query::Weight::PreparedWeight {
      QueryPrep::PreparedSource child;

    public:
      explicit Prepared(QueryPrep::PreparedSource&& child) : child(std::move(child)) {}

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        return QueryPrep::createScorer(targetPool, segment, child.segmentSource());
      }

      // Pass-through: matches and scores are the child's, so its supplier is too
      // (cost and get both delegate, no wrapping).
      Query::ScorerSupplier* scorerSupplier(MemPool& targetPool, IndexReader::Segment& segment) override {
        return child.segmentSource().scorerSupplier(targetPool, segment);
      }

      bool outputIsSubsetOfDomain() const noexcept override {
        return child.prepared != nullptr && child.prepared->outputIsSubsetOfDomain();
      }
    };

  public:
    Weight(Context& context, ForcePrepareQuery& query) : Query::Weight(context) {
      childWeight = query.child->createWeight(context);
    }

    bool needsPrepare() const noexcept override {
      return true;
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      QueryPrep::PreparedSource source;
      source.weight = childWeight;
      if (childWeight->needsPrepare()) {
        source.prepared = childWeight->prepare(ctx);
      }
      return std::make_unique<Prepared>(std::move(source));
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      return QueryPrep::createScorer(targetPool, segment, *childWeight);
    }

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool, IndexReader::Segment& segment) override {
      return childWeight->scorerSupplier(targetPool, segment);
    }
  };
};

} // namespace solux
