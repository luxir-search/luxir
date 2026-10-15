// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "Query.h"
#include "QueryPrep.h"

namespace luxir {

/// Minimum-score cut-off: matches the child's documents whose child score is
/// at least minScore, scored child * multiplier. Membership depends on child
/// scores, so the child is always built with NEED_SCORES and multiplier 1,
/// even when the wrapper itself is unscored (a filter position, a count, or a
/// field-sorted ranking), and the filter key is UNCACHEABLE.
///
/// The scorer is a plain exact cursor: next()/advance() skip below-threshold
/// child matches, so every consumer protocol (pull, window bits, candidate
/// sweeps) inherits the cut-off through the Scorer defaults. It exposes no
/// approximation enums, no docs-only or bulk products, and no constant count,
/// so every count, domain, and ranking route executes it. The child is never
/// switched into membership mode: verification always needs its score.
class MinScoreQuery final : public Query {
  Query* child;
  float minScore;

  class Scorer final : public Query::Scorer {
    Query::Scorer* child;
    float minScore;
    float multiplier;
    float current = 0.0f;
    int32_t doc = -1;
    bool scored;

    int32_t accept(int32_t candidate) {
      while (candidate != PostingsReader::END) {
        float childScore = child->score();
        // NaN compares false and is excluded.
        if (childScore >= minScore) {
          current = scored ? childScore * multiplier : 0.0f;
          break;
        }
        candidate = child->next();
      }
      doc = candidate;
      return doc;
    }

    // Matches are a subset of the child's, so its bounds stay valid. The
    // multiplier is finite and positive here; widening by one ulp keeps the
    // scaled interval conservative under rounding.
    ScoreBounds compose(ScoreBounds childBounds) const {
      if (!scored || multiplier == 0.0f) return ScoreBounds::exact(0.0f);
      if (multiplier == 1.0f) return childBounds;
      return {std::nextafter(childBounds.lo * multiplier,
                             -std::numeric_limits<float>::infinity()),
              std::nextafter(childBounds.hi * multiplier,
                             std::numeric_limits<float>::infinity())};
    }

  public:
    Scorer(Query::Scorer* child, float minScore, float multiplier, bool scored)
      : child(child), minScore(minScore), multiplier(multiplier),
        scored(scored) {}

    int32_t next() override { return accept(child->next()); }

    int32_t advance(int32_t target) override {
      assert(doc < target);
      return accept(child->advance(target));
    }

    int32_t docId() override { return doc; }

    float matchCost() override { return child->matchCost(); }

    float score() override { return current; }

    // The collector floor applies to the scaled score; the child sees it in
    // its own score space, rounded down so ties stay competitive, and never
    // below the cut-off it is going to be rejected by anyway.
    void setMinCompetitiveScore(float floor) override {
      if (!scored || multiplier == 0.0f) return;
      float childFloor = multiplier == 1.0f
          ? floor
          : std::nextafter(floor / multiplier,
                           -std::numeric_limits<float>::infinity());
      child->setMinCompetitiveScore(std::max(childFloor, minScore));
    }

    float getMaxScore(int32_t upTo) override {
      return getScoreBounds(upTo).hi;
    }

    float refineMaxScore(int32_t upTo) override {
      return refineScoreBounds(upTo).hi;
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return compose(child->getScoreBounds(upTo));
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return compose(child->refineScoreBounds(upTo));
    }

    int32_t advanceShallow(int32_t target) override {
      return child->advanceShallow(target);
    }
  };

  class Supplier final : public Query::ScorerSupplier {
    Query::ScorerSupplier* childSupplier;
    float minScore;
    float multiplier;
    bool scored;

    class Plan final : public Query::ScorerPlan {
      Query::ScorerPlan* childPlan;
      float minScore;
      float multiplier;
      bool scored;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        Query::Scorer* childScorer = childPlan->build(targetPool);
        if (childScorer == nullptr) return nullptr;
        return targetPool.make<Scorer>(childScorer, minScore, multiplier,
                                       scored);
      }

    public:
      Plan(const Query::PlanContext& planContext,
           const Query::ScorerShape& shape, int64_t cost,
           Query::ScorerPlan* childPlan, float minScore, float multiplier,
           bool scored)
        : Query::ScorerPlan(planContext, shape, cost), childPlan(childPlan),
          minScore(minScore), multiplier(multiplier), scored(scored) {}
    };

  public:
    Supplier(Query::ScorerSupplier* childSupplier, float minScore,
             float multiplier, bool scored)
      : childSupplier(childSupplier), minScore(minScore),
        multiplier(multiplier), scored(scored) {}

    int64_t cost() override { return childSupplier->cost(); }

    Query::ScorerShape describeScorer(
        const Query::PlanContext& buildContext) const override {
      Query::ScorerShape child = childSupplier->describeScorer(buildContext);
      return {
        .matchState = child.matchState,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = Query::ReportedTwoPhase::NO,
        .windowFillClause = Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
      };
    }

    Query::VerificationWork verificationWork(
        const Query::PlanContext& buildContext) const override {
      unused(buildContext);
      return Query::VerificationWork::PRESENT;
    }

    Query::UnresolvedSupplierCause unresolvedScorerCause(
        const Query::PlanContext& buildContext) const override {
      return childSupplier->unresolvedScorerCause(buildContext);
    }

    bool fillExpansionMemo(
        const Query::PlanContext& buildContext) override {
      return childSupplier->fillExpansionMemo(buildContext);
    }

    Query::PlanContext makePlanContext(
        const Query::Demand& demand) const override {
      return childSupplier->makePlanContext(demand);
    }

    Query::ScorerPlan* resolve(
        MemPool& planPool,
        const Query::PlanContext& planContext) override {
      // The child is always consumed through exact scored pull iteration.
      Query::PlanContext childContext = planContext;
      childContext.demand.horizon = Query::ExecutionUse::PULL;
      Query::ScorerPlan* childPlan =
          childSupplier->resolve(planPool, childContext);
      assert(childPlan != nullptr);
      return planPool.make<Plan>(
          planContext, describeScorer(planContext), childPlan->cost(),
          childPlan, minScore, multiplier, scored);
    }
  };

  static Query::ScorerSupplier* wrapSupplier(
      MemPool& targetPool, IndexReader::Segment& segment,
      Query::SegmentSource& childSource, float minScore, float multiplier,
      bool scored, Query::SupplierExecutionMode executionMode) {
    Query::ScorerSupplier* childSupplier =
        childSource.scorerSupplier(targetPool, segment, executionMode);
    if (childSupplier == nullptr) return nullptr;
    return targetPool.make<Supplier>(childSupplier, minScore, multiplier,
                                     scored);
  }

  static Query::Scorer* buildPull(MemPool& targetPool,
                                  Query::ScorerSupplier* supplier) {
    if (supplier == nullptr) return nullptr;
    Query::Demand demand = Query::Demand::fromLeadCost(
        std::numeric_limits<int64_t>::max());
    return supplier->resolve(
        targetPool, supplier->makePlanContext(demand))->build(targetPool);
  }

public:
  MinScoreQuery(Query* child, float minScore)
    : Query(QueryKind::MIN_SCORE), child(child), minScore(minScore) {
    if (!std::isfinite(minScore)) {
      throw std::invalid_argument("min_score must be finite");
    }
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const MinScoreQuery&>(other);
    return std::bit_cast<uint32_t>(minScore)
               == std::bit_cast<uint32_t>(rhs.minScore)
        && child->equals(*rhs.child);
  }

  uint64_t hashImpl() const override {
    return mixHash(mixHash(Query::hashImpl(), child->hash()),
                   std::bit_cast<uint32_t>(minScore));
  }

  Query* getChild() const { return child; }
  float getMinScore() const { return minScore; }

  std::optional<ValueEnvelope> requiredValueEnvelope(
      std::string_view field) const override {
    return child->requiredValueEnvelope(field);
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    checkedBoostProduct(multiplier, 1.0f);
    child->validateLogical(context, 1.0f);
  }

  // Membership depends on scores, which the structural key omits.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(out, ctx);
    return FilterKeyScope::UNCACHEABLE;
  }

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override {
    return context.pool.make<MinScoreQuery::Weight>(context, *this, flags,
                                                    multiplier);
  }

  class Weight final : public Query::Weight {
    Query::Weight* childWeight;
    float minScore;
    float multiplier;
    bool scored;

    class Prepared final : public Query::Weight::PreparedWeight {
      QueryPrep::PreparedSource child;
      float minScore;
      float multiplier;
      bool scored;

    public:
      Prepared(QueryPrep::PreparedSource&& child, float minScore,
               float multiplier, bool scored)
        : child(std::move(child)), minScore(minScore), multiplier(multiplier),
          scored(scored) {}

      Query::Scorer* createScorer(
          MemPool& targetPool, IndexReader::Segment& segment) override {
        return buildPull(targetPool, scorerSupplier(targetPool, segment));
      }

      Query::ScorerSupplier* scorerSupplierImpl(
          MemPool& targetPool, IndexReader::Segment& segment,
          Query::SupplierExecutionMode executionMode) override {
        return wrapSupplier(targetPool, segment, child.segmentSource(),
                            minScore, multiplier, scored, executionMode);
      }

      void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
        child.releaseSegmentState(segment);
      }

      bool outputIsSubsetOfDomain() const noexcept override {
        return child.prepared != nullptr
            && child.prepared->outputIsSubsetOfDomain();
      }

      PreparedDomainDependence domainDependence() const noexcept override {
        return child.domainDependence;
      }
    };

  public:
    Weight(Context& context, MinScoreQuery& query, int32_t flags,
           float multiplier)
      : Query::Weight(context, query, flags), minScore(query.minScore),
        multiplier(multiplier), scored((flags & NEED_SCORES) != 0) {
      childWeight = query.child->createWeight(
          context, flags | NEED_SCORES, 1.0f);
      // Only the prepare requirement carries over: match-all, constant
      // counts, and count/top-k composition all describe the child's
      // unfiltered membership.
      traits = childWeight->getFlags() & NEEDS_PREPARE;
      if (!scored) traits |= IS_CONSTANT_SCORING;
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(
        Query::Weight::PrepareContext& ctx) override {
      QueryPrep::PreparedSource source;
      source.weight = childWeight;
      if (childWeight->needsPrepare()) {
        source.setPrepared(childWeight->prepare(ctx));
      }
      return std::make_unique<Prepared>(std::move(source), minScore,
                                        multiplier, scored);
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return buildPull(targetPool, scorerSupplier(targetPool, segment));
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      return wrapSupplier(targetPool, segment, *childWeight, minScore,
                          multiplier, scored, executionMode);
    }

    void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
      childWeight->releaseSegmentState(segment);
    }

    bool hasReleasableSegmentState() const noexcept override {
      return childWeight->hasReleasableSegmentState();
    }
  };
};

} // namespace luxir
