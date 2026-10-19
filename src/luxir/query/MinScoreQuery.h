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
/// The fixed child floor is exact for membership: anything pruned below it
/// would fail verification anyway. Pull execution owns the child's two-phase
/// protocol (or delegates its external approximation enums), retaining scores
/// even during bounded membership fills. Bulk execution filters the child's
/// scored windows in place; membership never uses a child's docs-only product.
class MinScoreQuery final : public Query {
  Query* child;
  float minScore;

  static float childFloor(float floor, float minScore, float multiplier,
                          bool scored) {
    if (!scored || multiplier == 0.0f) return minScore;
    float translated = multiplier == 1.0f
        ? floor
        : std::nextafter(floor / multiplier,
                         -std::numeric_limits<float>::infinity());
    return std::max(translated, minScore);
  }

  class Scorer final : public Query::Scorer {
    Query::Scorer* child;
    float minScore;
    float multiplier;
    float current = 0.0f;
    int32_t doc = -1;
    bool scored;

    bool acceptScore() {
      skipCount(SkipStats::minScoreChecks);
      float childScore = child->score();
      // NaN compares false and is excluded.
      if (!(childScore >= minScore)) return false;
      current = scored ? childScore * multiplier : 0.0f;
      return true;
    }

    int32_t accept(int32_t candidate) {
      while (candidate != PostingsReader::END && !matches()) {
        candidate = approximationNext();
      }
      return candidate;
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
        scored(scored) {
      child->setMinCompetitiveScore(minScore);
    }

    int32_t next() override { return accept(approximationNext()); }

    int32_t advance(int32_t target) override {
      assert(doc < target);
      return accept(approximationAdvance(target));
    }

    int32_t docId() override { return doc; }

    int32_t approximationNext() override {
      return doc = child->approximationNext();
    }

    int32_t approximationAdvance(int32_t target) override {
      return doc = child->approximationAdvance(target);
    }

    int32_t approximationDocId() override { return doc; }

    std::span<DocsPosEnum*> approximationEnums() override {
      return child->approximationEnums();
    }

    bool matches() override { return child->matches() && acceptScore(); }

    bool matchesAt(int32_t target) override {
      doc = target;
      return child->matchesAt(target) && acceptScore();
    }

    // Approximate a scalar score evaluation (norm/frequency lookup and score
    // arithmetic) in addition to the child's verifier.
    float matchCost() override { return child->matchCost() + 10.0f; }

    [[gnu::cold]] void prepareMembership() override {
      // Keep the child's scoring state: membership still needs its score.
    }

    int32_t advanceApproximationWindow(int32_t min, int32_t max) override {
      if (min >= max) return max;
      // An exact child approximation still follows its own single-phase rules.
      if (doc < min) {
        return min == doc + 1 ? approximationNext() : approximationAdvance(min);
      }
      return doc;
    }

    int32_t advanceWindow(int32_t min, int32_t max) override {
      if (min >= max) return max;
      int32_t candidate = advanceApproximationWindow(min, max);
      while (candidate < max) {
        if (matches()) return candidate;
        candidate = approximationNext();
      }
      return candidate;
    }

    int32_t applyToCandidates(int32_t* docs, float* scores,
                              int32_t size, bool required) override {
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        int32_t target = docs[i];
        bool matched = advanceWindow(target, target + 1) == target;
        if (matched) scores[i] += current;
        if (required && matched) {
          docs[write] = target;
          scores[write++] = scores[i];
        }
      }
      return required ? write : size;
    }

    float score() override { return current; }

    // The collector floor applies to the scaled score; the child sees it in
    // its own score space, rounded down so ties stay competitive, and never
    // below the cut-off it is going to be rejected by anyway.
    void setMinCompetitiveScore(float floor) override {
      if (!scored || multiplier == 0.0f) return;
      child->setMinCompetitiveScore(
          childFloor(floor, minScore, multiplier, scored));
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

  class BulkScorer final : public luxir::BulkScorer {
    luxir::BulkScorer* child;
    float minScore;
    float multiplier;
    bool scored;

    void compact(ScoreWindow& out) {
      skipCount(SkipStats::minScoreBulkWindows);
      if (SkipStats::enabled) SkipStats::minScoreChecks += out.size;
      int32_t write = 0;
      for (int32_t i = 0; i < out.size; i++) {
        float score = out.scores[(size_t)i];
        // A bulk floor permits pruning, but need not filter individual docs.
        if (!(score >= minScore)) continue;
        out.docs[(size_t)write] = out.docs[(size_t)i];
        out.scores[(size_t)write++] = scored ? score * multiplier : 0.0f;
      }
      out.size = write;
    }

  public:
    BulkScorer(luxir::BulkScorer* child, float minScore, float multiplier,
               bool scored)
      : child(child), minScore(minScore), multiplier(multiplier),
        scored(scored) {}

    void setTopKDepth(int32_t topK, bool allowPruning) override {
      unused(allowPruning);
      // The fixed floor licenses pruning even for exact counts. Fresh
      // membership products get no depth call; child scored-window defaults
      // already honor the floor (conjunction competitivePruning starts true).
      child->setTopKDepth(topK, true);
    }

    [[gnu::cold]] void prepareMembership() override {
      // Membership continues consuming scored child windows.
    }

    bool supportsMatchWindows() const override { return true; }

    bool attachWindowFilter(WindowFilter* filter) override {
      return child->attachWindowFilter(filter);
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max, float theta) override {
      int32_t next = child->scoreNextWindow(
          out, filter, min, max,
          childFloor(theta, minScore, multiplier, scored));
      compact(out);
      return next;
    }

    int32_t matchNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max) override {
      int32_t next = child->scoreNextWindow(out, filter, min, max, minScore);
      compact(out);
      return next;
    }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      ScoreWindow out;
      int32_t next = matchNextWindow(out, filter, min, max);
      count += out.size;
      if (domainOut != nullptr) {
        skipCount(SkipStats::bulkDomainWindowsFed);
        domainOut->addSorted(out.docs.first((size_t)out.size));
      }
      return next;
    }
  };

  class Supplier final : public Query::ScorerSupplier {
    MemPool& pool;
    Query::ScorerSupplier* childSupplier;
    float minScore;
    float multiplier;
    bool scored;

    struct BulkState : BulkBuildState {
      BulkPlan childPlan;

      BulkState(Supplier* supplier, const BulkPlan& childPlan)
        : BulkBuildState{supplier}, childPlan(childPlan) {}
    };

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
    Supplier(MemPool& pool, Query::ScorerSupplier* childSupplier, float minScore,
             float multiplier, bool scored)
      : pool(pool), childSupplier(childSupplier), minScore(minScore),
        multiplier(multiplier), scored(scored) {}

    int64_t cost() override { return childSupplier->cost(); }

    Query::ScorerShape describeScorer(
        const Query::PlanContext& buildContext) const override {
      Query::ScorerShape child = childSupplier->describeScorer(buildContext);
      return {
        .matchState = child.matchState,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = Query::ReportedTwoPhase::YES,
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
      // The child retains scoring even when our consumer requests membership.
      Query::PlanContext childContext = planContext;
      childContext.demand.horizon = Query::ExecutionUse::PULL;
      Query::ScorerPlan* childPlan =
          childSupplier->resolve(planPool, childContext);
      assert(childPlan != nullptr);
      return planPool.make<Plan>(
          planContext, describeScorer(planContext), childPlan->cost(),
          childPlan, minScore, multiplier, scored);
    }

    BulkPlan planBulk(
        BulkUse use, const BulkScorerContext& bulkContext) override {
      if (bulkContext.requireConstantCount
          || (use != BulkUse::SCORED_WINDOWS
              && use != BulkUse::MATCH_WINDOWS
              && use != BulkUse::COUNT_WINDOWS)) return {};
      BulkPlan childPlan = childSupplier->planBulk(
          BulkUse::SCORED_WINDOWS, bulkContext);
      auto* state = pool.make<BulkState>(this, childPlan);
      BulkPlan plan;
      plan.buildState = state;
      if (childPlan.available == BulkAnswer::YES
          && !childPlan.hasConstantCount()) {
        plan.available = BulkAnswer::YES;
        plan.supportsMatchWindows = BulkAnswer::YES;
        plan.consumesFilters = childPlan.consumesFilters;
        plan.acceptsWindowFilter = childPlan.acceptsWindowFilter;
      }
      return plan;
    }

    void recordBulkPlanCommitment(
        BulkUse use, const BulkScorerContext& bulkContext,
        const BulkPlan& plan) override {
      unused(use);
      if (plan.buildState == nullptr) return;
      assert(plan.buildState->owner == this);
      const auto& state = *static_cast<const BulkState*>(plan.buildState);
      childSupplier->recordBulkPlanCommitment(
          BulkUse::SCORED_WINDOWS, bulkContext, state.childPlan);
    }

    luxir::BulkScorer* buildBulk(
        MemPool& targetPool, const BulkPlan& plan) override {
      assert(plan.available == BulkAnswer::YES
             && plan.buildState != nullptr && plan.buildState->owner == this);
      const auto& state = *static_cast<const BulkState*>(plan.buildState);
      auto* child = childSupplier->buildBulk(targetPool, state.childPlan);
      if (child == nullptr) return nullptr;
      return targetPool.make<BulkScorer>(child, minScore, multiplier, scored);
    }
  };

  static Query::ScorerSupplier* wrapSupplier(
      MemPool& targetPool, IndexReader::Segment& segment,
      Query::SegmentSource& childSource, float minScore, float multiplier,
      bool scored, Query::SupplierExecutionMode executionMode) {
    Query::ScorerSupplier* childSupplier =
        childSource.scorerSupplier(targetPool, segment, executionMode);
    if (childSupplier == nullptr) return nullptr;
    return targetPool.make<Supplier>(targetPool, childSupplier, minScore, multiplier,
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
                       float multiplier = 1.0f) override;

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
          context, (flags & ~EXCLUSION_WINDOW_FILL) | NEED_SCORES | ALLOW_PRUNING,
          1.0f);
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
