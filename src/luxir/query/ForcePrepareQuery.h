#pragma once

#include <utility>

#include "Query.h"
#include "QueryPrep.h"

namespace luxir {

// Debug/testing wrapper that forces a child query through prepare() without
// changing its matches or scores.
class ForcePrepareQuery final : public luxir::Query {
  Query* child;

public:
  explicit ForcePrepareQuery(Query* child) : child(child) {}

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    return child->appendFilterKey(out, ctx);
  }

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override {
    return context.pool.make<ForcePrepareQuery::Weight>(context, *this, flags,
                                                        multiplier);
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

      PreparedDomainDependence domainDependence() const noexcept override {
        return child.domainDependence;
      }
    };

  public:
    Weight(Context& context, ForcePrepareQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags) {
      childWeight = query.child->createWeight(context, flags, multiplier);
      // Forces prepare; scoring behavior is otherwise the child's.
      traits |= NEEDS_PREPARE | (childWeight->getFlags() & IS_CONSTANT_SCORING);
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      QueryPrep::PreparedSource source;
      source.weight = childWeight;
      if (childWeight->needsPrepare()) {
        source.setPrepared(childWeight->prepare(ctx));
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

} // namespace luxir
