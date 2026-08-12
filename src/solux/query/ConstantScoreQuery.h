#pragma once

#include <memory>
#include <utility>

#include "Query.h"
#include "QueryPrep.h"

namespace solux {

// Wrapper that preserves a child query's matching docs but assigns the same
// score to each hit. This is also a small example of the compound-query
// prepare contract: create a child Weight, forward needsPrepare(), and wrap
// either the live child Weight or its PreparedWeight at scorer creation time.
class ConstantScoreQuery final : public solux::Query {
  Query* child;
  float constantScore;

  // setMinCompetitiveScore is not forwarded to the child: it was built
  // scoreless, so the wrapper owns all score semantics. The exhaustion
  // latch is honored here (unlike leaf constant scorers) because the branch
  // gates a child call, not a leaf's hot decode loop, and ending iteration
  // skips all remaining child work.
  class Scorer final : public Query::ConstantScorer {
    Query::Scorer* child;
    bool exhausted = false;

    void exhaust() override { exhausted = true; }

  public:
    Scorer(Query::Scorer* child, float constantScore)
      : Query::ConstantScorer(constantScore), child(child) {}

    int32_t next() override {
      return exhausted ? PostingsReader::END : child->next();
    }

    int32_t advance(int32_t docid) override {
      return exhausted ? PostingsReader::END : child->advance(docid);
    }

    int32_t docId() override {
      return child->docId();
    }

    // Matching is exactly the child's, so forward two-phase iteration: a
    // constant_score(range) / constant_score(phrase) clause keeps verifying
    // cheaply in a conjunction instead of forcing its child to iterate fully.
    int32_t approximationNext() override {
      return exhausted ? PostingsReader::END : child->approximationNext();
    }
    int32_t approximationAdvance(int32_t target) override {
      return exhausted ? PostingsReader::END : child->approximationAdvance(target);
    }
    int32_t approximationDocId() override { return child->approximationDocId(); }
    bool matches() override { return child->matches(); }
    float matchCost() override { return child->matchCost(); }
  };

  // Delegates cost to the child supplier and wraps its scorer with the constant
  // score, so a constant_score(...) clause reports the child's real cost to a
  // parent's cost-based planning instead of the default maxDoc.
  class Supplier final : public Query::ScorerSupplier {
    Query::ScorerSupplier* childSupplier;
    float constantScore;

    static Query::ScorerShape wrappedShape(
        const Query::ScorerShape& child) {
      return {
        .matchState = child.matchState,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = child.reportedTwoPhase,
        .windowFillClause = Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
      };
    }

    class Plan final : public Query::ScorerPlan {
      Query::ScorerPlan* childPlan;
      float constantScore;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        Query::Scorer* childScorer = childPlan->build(targetPool);
        if (childScorer == nullptr) return nullptr;
        return targetPool.make<ConstantScoreQuery::Scorer>(
            childScorer, constantScore);
      }

    public:
      Plan(Supplier& supplier, const Query::PlanContext& planContext,
           const Query::ScorerShape& shape, int64_t cost,
           Query::ScorerPlan* childPlan, float constantScore)
        : Query::ScorerPlan(planContext, shape, cost),
          childPlan(childPlan), constantScore(constantScore) {}
    };

  public:
    Supplier(Query::ScorerSupplier* childSupplier, float constantScore)
      : childSupplier(childSupplier), constantScore(constantScore) {}

    int64_t cost() override { return childSupplier->cost(); }

    Query::ScorerShape describeScorer(
        const Query::PlanContext& buildContext) const override {
      return wrappedShape(childSupplier->describeScorer(buildContext));
    }

    Query::UnresolvedSupplierCause unresolvedScorerCause(
        const Query::PlanContext& buildContext) const override {
      return childSupplier->unresolvedScorerCause(buildContext);
    }

    bool fillExpansionMemo(
        const Query::PlanContext& buildContext) override {
      return childSupplier->fillExpansionMemo(buildContext);
    }

    Query::ScorerPlan* resolve(
        MemPool& planPool,
        const Query::PlanContext& planContext) override {
      Query::ScorerPlan* childPlan =
          childSupplier->resolve(planPool, planContext);
      assert(childPlan != nullptr);
      return planPool.make<Plan>(
          *this, planContext, wrappedShape(childPlan->shape()),
          childPlan->cost(), childPlan, constantScore);
    }

    Query::PlanContext makePlanContext(
        const Query::Demand& demand) const override {
      return childSupplier->makePlanContext(demand);
    }

    BulkPlan planBulk(
        BulkUse use, const BulkScorerContext& bulkContext) override {
      if (!bulkContext.requireConstantCount) {
        return {
          BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
          BulkAnswer::NO,
        };
      }
      return childSupplier->planBulk(use, bulkContext);
    }
  };

public:
  ConstantScoreQuery(Query* child, float constantScore = 1.0f) : child(child), constantScore(constantScore) {}

  Query* getChild() const { return child; }
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    return child->appendFilterKey(out, ctx);
  }
  ScoreProfile scoreProfile() const override {
    return ScoreProfile::explicitUniform(constantScore);
  }

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override {
    return context.pool.make<ConstantScoreQuery::Weight>(context, *this, flags,
                                                         multiplier);
  }

  class Weight final : public Query::Weight {
    Query::Weight* childWeight = nullptr;
    float constantScore;

    class Prepared final : public Query::Weight::PreparedWeight {
      QueryPrep::PreparedSource child;
      float constantScore;

    public:
      Prepared(QueryPrep::PreparedSource&& child, float constantScore)
        : child(std::move(child)), constantScore(constantScore) {}

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        auto* childScorer = QueryPrep::createScorer(targetPool, segment, child.segmentSource());
        if (childScorer == nullptr) return nullptr;
        return targetPool.make<ConstantScoreQuery::Scorer>(childScorer, constantScore);
      }

      Query::ScorerSupplier* scorerSupplier(MemPool& targetPool, IndexReader::Segment& segment) override {
        auto* childSupplier = child.segmentSource().scorerSupplier(targetPool, segment);
        if (childSupplier == nullptr) return nullptr;
        return targetPool.make<ConstantScoreQuery::Supplier>(childSupplier, constantScore);
      }

      bool outputIsSubsetOfDomain() const noexcept override {
        return child.prepared != nullptr && child.prepared->outputIsSubsetOfDomain();
      }

      PreparedDomainDependence domainDependence() const noexcept override {
        return child.domainDependence;
      }
    };

  public:
    Weight(Context& context, ConstantScoreQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags),
        constantScore(constantWhenScored(flags, multiplier, query.constantScore)) {
      // The child constrains matches; this wrapper replaces its score.
      childWeight = query.child->createWeight(context, flags & ~NEED_SCORES, 1.0f);
      // The wrapper is constant-scoring; prepare still follows the child.
      traits |= IS_CONSTANT_SCORING | (childWeight->getFlags() & NEEDS_PREPARE);
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      QueryPrep::PreparedSource source;
      source.weight = childWeight;
      if (childWeight->needsPrepare()) {
        source.setPrepared(childWeight->prepare(ctx));
      }
      return std::make_unique<Prepared>(std::move(source), constantScore);
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      auto* childScorer = QueryPrep::createScorer(targetPool, segment, *childWeight);
      if (childScorer == nullptr) return nullptr;
      return targetPool.make<ConstantScoreQuery::Scorer>(childScorer, constantScore);
    }

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool, IndexReader::Segment& segment) override {
      auto* childSupplier = childWeight->scorerSupplier(targetPool, segment);
      if (childSupplier == nullptr) return nullptr;
      return targetPool.make<ConstantScoreQuery::Supplier>(childSupplier, constantScore);
    }
  };
};

} // namespace solux
