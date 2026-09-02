#pragma once

#include <atomic>
#include <utility>

#include "Query.h"
#include "QueryPrep.h"

namespace luxir {

// Debug/testing wrapper that forces a child query through prepare() without
// changing its matches or scores.
class ForcePrepareQuery final : public luxir::Query {
  Query* child;

public:
  static inline std::atomic<int64_t> prepareCallsForTests{0};

  explicit ForcePrepareQuery(Query* child) : child(child) {}

  bool equals(const Query& other) const override {
    const auto* rhs = dynamic_cast<const ForcePrepareQuery*>(&other);
    return rhs != nullptr && sameScoringClause(
        child, rhs->child);
  }

  uint64_t hashImpl() const override {
    return mixHash(Query::hashImpl(), scoringClauseHash(child));
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    child->validateLogical(context, multiplier);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    return child->appendFilterKey(out, ctx);
  }

  bool exactDomainIdentity() const override {
    return child->exactDomainIdentity();
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
      Query::ScorerSupplier* scorerSupplierImpl(
          MemPool& targetPool, IndexReader::Segment& segment,
          Query::SupplierExecutionMode executionMode) override {
        return child.segmentSource().scorerSupplier(
            targetPool, segment, executionMode);
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
      : Query::Weight(context, query, flags) {
      childWeight = query.child->createWeight(context, flags, multiplier);
      // Forces prepare; scoring behavior is otherwise the child's.
      traits |= NEEDS_PREPARE | (childWeight->getFlags() & IS_CONSTANT_SCORING);
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      prepareCallsForTests.fetch_add(1, std::memory_order_relaxed);
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

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      return childWeight->scorerSupplier(
          targetPool, segment, executionMode);
    }

    std::optional<int64_t> constantCount(
        IndexReader::Segment& segment, DocSet* domain) override {
      return childWeight->constantCount(segment, domain);
    }
  };
};

} // namespace luxir
