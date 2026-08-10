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
    bool hasTwoPhase() const override { return child->hasTwoPhase(); }
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
  public:
    Supplier(Query::ScorerSupplier* childSupplier, float constantScore)
      : childSupplier(childSupplier), constantScore(constantScore) {}

    int64_t cost() override { return childSupplier->cost(); }

    Query::ScorerShape describeScorer(
        const Query::ScorerBuildContext& buildContext) const override {
      Query::ScorerShape child = childSupplier->describeScorer(buildContext);
      return {
        .matchState = child.matchState,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = child.reportedTwoPhase,
        .windowFillClause = Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
      };
    }

    Query::UnresolvedSupplierCause unresolvedScorerCause(
        const Query::ScorerBuildContext& buildContext) const override {
      return childSupplier->unresolvedScorerCause(buildContext);
    }

    bool fillExpansionMemo(
        const Query::ScorerBuildContext& buildContext) override {
      return childSupplier->fillExpansionMemo(buildContext);
    }

    Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
      auto* childScorer = childSupplier->get(targetPool, leadCost);
      if (childScorer == nullptr) return nullptr;
      return targetPool.make<ConstantScoreQuery::Scorer>(childScorer, constantScore);
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
