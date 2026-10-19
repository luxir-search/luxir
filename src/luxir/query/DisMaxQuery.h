// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "Query.h"
#include "QueryPrep.h"

namespace luxir {

// Disjunction max (Lucene's DisjunctionMaxQuery, Elasticsearch dis_max): a
// document matches when any disjunct matches, and scores
//
//   max_i s_i + tieBreaker * (sum_i s_i - max_i s_i)
//
// over the disjuncts that match it. tieBreaker 0 takes the best disjunct
// alone; 1 adds them up like a disjunction.
//
// Membership is exactly the disjunction of the disjuncts, so the query holds
// that BooleanQuery: unscored use and the filter key go through it and share
// the boolean's cache entries. Scored use merges the disjuncts' pull scorers.
class DisMaxQuery final : public Query {
  std::span<Query*> disjuncts;
  float tieBreaker;
  Query* membership;

  static float roundUp(float value) {
    return std::nextafter(value, std::numeric_limits<float>::infinity());
  }

  static float roundDown(float value) {
    return std::nextafter(value, -std::numeric_limits<float>::infinity());
  }

public:
  // membership must be the SHOULD-only BooleanQuery over the same disjuncts;
  // make() builds both.
  DisMaxQuery(std::span<Query*> disjuncts, float tieBreaker, Query* membership)
    : Query(QueryKind::DIS_MAX), disjuncts(disjuncts), tieBreaker(tieBreaker),
      membership(membership) {
    if (!std::isfinite(tieBreaker) || tieBreaker < 0.0f || tieBreaker > 1.0f) {
      throw std::invalid_argument("dis_max tie_breaker must be in [0, 1]");
    }
    assert(disjuncts.size() >= 2);
  }

  // A single disjunct is returned as is: its max is its own score.
  static Query* make(MemPool& pool, std::span<Query*> disjuncts, float tieBreaker);

  std::span<Query*> getDisjuncts() const { return disjuncts; }
  float getTieBreaker() const { return tieBreaker; }
  Query* getMembership() const { return membership; }

  bool recoverableAsPredicate() const noexcept override {
    return membership->recoverableAsPredicate();
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const DisMaxQuery&>(other);
    if (std::bit_cast<uint32_t>(tieBreaker) != std::bit_cast<uint32_t>(rhs.tieBreaker)
        || disjuncts.size() != rhs.disjuncts.size()) {
      return false;
    }
    for (size_t i = 0; i < disjuncts.size(); i++) {
      if (!disjuncts[i]->equals(*rhs.disjuncts[i])) return false;
    }
    return true;
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), std::bit_cast<uint32_t>(tieBreaker));
    for (auto* disjunct : disjuncts) value = mixHash(value, disjunct->hash());
    return value;
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    float product = checkedBoostProduct(multiplier, 1.0f);
    for (auto* disjunct : disjuncts) disjunct->validateLogical(context, product);
  }

  // Scores never change which documents match.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    return membership->appendFilterKey(out, ctx);
  }

  class Scorer final : public Query::Scorer {
    struct Slot {
      Query::Scorer* scorer;
      bool twoPhase;
      bool matched;
      int32_t doc;

      int32_t next() {
        doc = twoPhase ? scorer->approximationNext() : scorer->next();
        return doc;
      }

      int32_t advance(int32_t target) {
        doc = twoPhase ? scorer->approximationAdvance(target) : scorer->advance(target);
        return doc;
      }
    };

    std::span<Slot> slots;
    float tieBreaker;
    float verificationCost = 0.0f;
    int32_t doc = -1;

    int32_t settle() {
      int32_t minDoc = PostingsReader::END;
      for (auto& slot : slots) minDoc = std::min(minDoc, slot.doc);
      return doc = minDoc;
    }

    int32_t nextMatched(int32_t candidate) {
      while (candidate != PostingsReader::END && !matches()) {
        candidate = approximationNext();
      }
      return candidate;
    }

    // score <= (1 - t) * max_i hi_i + t * sum_i max(hi_i, 0), and
    // score >= (1 - t) * min_i lo_i + t * sum_i min(lo_i, 0): at least one
    // disjunct matches, and an absent one contributes 0 to the sum.
    ScoreBounds combine(int32_t upTo, bool refined) {
      float hiMax = -std::numeric_limits<float>::infinity();
      float loMin = std::numeric_limits<float>::infinity();
      double hiSum = 0.0;
      double loSum = 0.0;
      bool any = false;
      for (auto& slot : slots) {
        if (slot.doc == PostingsReader::END || slot.doc > upTo) continue;
        ScoreBounds b = refined ? slot.scorer->refineScoreBounds(upTo)
                                : slot.scorer->getScoreBounds(upTo);
        hiMax = std::max(hiMax, b.hi);
        loMin = std::min(loMin, b.lo);
        hiSum += std::max(b.hi, 0.0f);
        loSum += std::min(b.lo, 0.0f);
        any = true;
      }
      if (!any) return ScoreBounds::exact(0.0f);
      double t = tieBreaker;
      float hi = std::isinf(hiMax) || std::isinf(hiSum)
          ? hiMax : (float)((1.0 - t) * hiMax + t * hiSum);
      float lo = std::isinf(loMin) || std::isinf(loSum)
          ? loMin : (float)((1.0 - t) * loMin + t * loSum);
      return {roundDown(lo), roundUp(hi)};
    }

  public:
    Scorer(std::span<Slot> slots, float tieBreaker)
      : slots(slots), tieBreaker(tieBreaker) {
      for (auto& slot : slots) {
        if (slot.twoPhase) {
          verificationCost = std::max(verificationCost, slot.scorer->matchCost());
        }
      }
    }

    static Scorer* create(MemPool& pool, std::span<Query::Scorer*> scorers,
                          std::span<const uint8_t> twoPhase, float tieBreaker) {
      auto slots = pool.make_span<Slot>(scorers.size());
      for (size_t i = 0; i < scorers.size(); i++) {
        bool approx = twoPhase[i] != 0;
        int32_t at = approx ? scorers[i]->approximationDocId() : scorers[i]->docId();
        slots[i] = {scorers[i], approx, false, at};
      }
      return pool.make<Scorer>(slots, tieBreaker);
    }

    int32_t docId() override { return doc; }
    int32_t approximationDocId() override { return doc; }

    int32_t next() override { return nextMatched(approximationNext()); }

    int32_t advance(int32_t target) override {
      assert(doc < target);
      return nextMatched(approximationAdvance(target));
    }

    int32_t approximationNext() override {
      assert(doc != PostingsReader::END);
      for (auto& slot : slots) {
        if (slot.doc <= doc) slot.next();
      }
      return settle();
    }

    int32_t approximationAdvance(int32_t target) override {
      assert(doc < target);
      for (auto& slot : slots) {
        if (slot.doc < target) slot.advance(target);
      }
      return settle();
    }

    // Verifies every disjunct on the doc, since the score needs them all.
    bool matches() override {
      bool any = false;
      for (auto& slot : slots) {
        slot.matched = slot.doc == doc && (!slot.twoPhase || slot.scorer->matches());
        any |= slot.matched;
      }
      return any;
    }

    float matchCost() override { return verificationCost; }

    int32_t advanceApproximationWindow(int32_t min, int32_t max) override {
      if (min >= max) return max;
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

    float score() override {
      float best = 0.0f;
      float sum = 0.0f;
      bool any = false;
      for (auto& slot : slots) {
        if (!slot.matched) continue;
        float s = slot.scorer->score();
        best = any ? std::max(best, s) : s;
        sum += s;
        any = true;
      }
      return best + tieBreaker * (sum - best);
    }

    int32_t advanceShallow(int32_t target) override {
      int32_t upTo = PostingsReader::END;
      for (auto& slot : slots) {
        if (slot.doc != PostingsReader::END) {
          upTo = std::min(upTo, slot.scorer->advanceShallow(target));
        }
      }
      return upTo;
    }

    float getMaxScore(int32_t upTo) override { return combine(upTo, false).hi; }
    float refineMaxScore(int32_t upTo) override { return combine(upTo, true).hi; }
    ScoreBounds getScoreBounds(int32_t upTo) override { return combine(upTo, false); }
    ScoreBounds refineScoreBounds(int32_t upTo) override { return combine(upTo, true); }

    [[gnu::cold]] void prepareMembership() override {
      for (auto& slot : slots) slot.scorer->prepareMembership();
    }
  };

  class Supplier final : public Query::ScorerSupplier {
    std::span<Query::ScorerSupplier*> children;
    float tieBreaker;

    class Plan final : public Query::ScorerPlan {
      std::span<Query::ScorerPlan*> childPlans;
      float tieBreaker;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        auto scorers = targetPool.make_span<Query::Scorer*>(childPlans.size());
        auto twoPhase = targetPool.make_span<uint8_t>(childPlans.size());
        size_t n = 0;
        for (auto* plan : childPlans) {
          bool approx = plan->shape().reportedTwoPhase != Query::ReportedTwoPhase::NO;
          Query::Scorer* scorer = plan->build(targetPool);
          if (scorer == nullptr) continue;
          twoPhase[n] = approx ? 1 : 0;
          scorers[n++] = scorer;
        }
        if (n == 0) return nullptr;
        if (n == 1) return scorers[0];
        return Scorer::create(targetPool, scorers.first(n), twoPhase.first(n), tieBreaker);
      }

    public:
      Plan(const Query::PlanContext& planContext, const Query::ScorerShape& shape,
           int64_t cost, std::span<Query::ScorerPlan*> childPlans, float tieBreaker)
        : Query::ScorerPlan(planContext, shape, cost), childPlans(childPlans),
          tieBreaker(tieBreaker) {}
    };

  public:
    Supplier(std::span<Query::ScorerSupplier*> children, float tieBreaker)
      : children(children), tieBreaker(tieBreaker) {}

    int64_t cost() override {
      int64_t total = 0;
      for (auto* child : children) total += child->cost();
      return total;
    }

    Query::ScorerShape describeScorer(
        const Query::PlanContext& buildContext) const override {
      Query::ReportedTwoPhase twoPhase = Query::ReportedTwoPhase::NO;
      for (auto* child : children) {
        auto reported = child->describeScorer(buildContext).reportedTwoPhase;
        if (reported != Query::ReportedTwoPhase::NO) twoPhase = Query::ReportedTwoPhase::YES;
      }
      return {
        .matchState = Query::MatchState::NONEMPTY,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = twoPhase,
        .windowFillClause = Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
      };
    }

    Query::UnresolvedSupplierCause unresolvedScorerCause(
        const Query::PlanContext& buildContext) const override {
      for (auto* child : children) {
        auto cause = child->unresolvedScorerCause(buildContext);
        if (cause != Query::UnresolvedSupplierCause::NONE) return cause;
      }
      return Query::UnresolvedSupplierCause::NONE;
    }

    bool fillExpansionMemo(const Query::PlanContext& buildContext) override {
      bool filled = false;
      for (auto* child : children) filled |= child->fillExpansionMemo(buildContext);
      return filled;
    }

    Query::ScorerPlan* resolve(
        MemPool& planPool, const Query::PlanContext& planContext) override {
      // Every disjunct scores, whatever window protocol our consumer uses.
      Query::PlanContext childContext = planContext;
      childContext.demand.horizon = Query::ExecutionUse::PULL;
      auto plans = planPool.make_span<Query::ScorerPlan*>(children.size());
      for (size_t i = 0; i < children.size(); i++) {
        plans[i] = children[i]->resolve(planPool, childContext);
      }
      return planPool.make<Plan>(planContext, describeScorer(planContext), cost(),
                                 plans, tieBreaker);
    }
  };

  static Query::ScorerSupplier* makeSupplier(
      MemPool& targetPool, IndexReader::Segment& segment,
      std::span<Query::SegmentSource* const> sources, float tieBreaker,
      Query::SupplierExecutionMode executionMode) {
    auto children = targetPool.make_span<Query::ScorerSupplier*>(sources.size());
    size_t n = 0;
    for (auto* source : sources) {
      auto* child = source->scorerSupplier(targetPool, segment, executionMode);
      if (child != nullptr) children[n++] = child;
    }
    if (n == 0) return nullptr;
    if (n == 1) return children[0];
    return targetPool.make<Supplier>(children.first(n), tieBreaker);
  }

  static Query::Scorer* buildPull(MemPool& targetPool, Query::ScorerSupplier* supplier) {
    if (supplier == nullptr) return nullptr;
    Query::Demand demand = Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max());
    return supplier->resolve(targetPool, supplier->makePlanContext(demand))->build(targetPool);
  }

  class Weight final : public Query::Weight {
    std::span<Query::Weight*> children;
    std::span<Query::SegmentSource*> sources;
    float tieBreaker;

    class Prepared final : public Query::Weight::PreparedWeight {
      std::vector<QueryPrep::PreparedSource> children;
      std::vector<Query::SegmentSource*> sources;
      float tieBreaker;

    public:
      Prepared(std::vector<QueryPrep::PreparedSource>&& prepared, float tieBreaker)
        : children(std::move(prepared)), tieBreaker(tieBreaker) {
        for (auto& child : children) sources.push_back(&child.segmentSource());
      }

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        return buildPull(targetPool, scorerSupplier(targetPool, segment));
      }

      Query::ScorerSupplier* scorerSupplierImpl(
          MemPool& targetPool, IndexReader::Segment& segment,
          Query::SupplierExecutionMode executionMode) override {
        return makeSupplier(targetPool, segment, sources, tieBreaker, executionMode);
      }

      void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
        for (auto& child : children) child.releaseSegmentState(segment);
      }

      bool outputIsSubsetOfDomain() const noexcept override {
        for (auto& child : children) {
          if (child.prepared == nullptr || !child.prepared->outputIsSubsetOfDomain()) {
            return false;
          }
        }
        return true;
      }

      PreparedDomainDependence domainDependence() const noexcept override {
        auto dependence = PreparedDomainDependence::QUERY_CANONICAL;
        for (auto& child : children) {
          dependence = std::max(dependence, child.domainDependence);
        }
        return dependence;
      }
    };

  public:
    Weight(Context& context, DisMaxQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, query, flags), tieBreaker(query.tieBreaker) {
      size_t n = query.disjuncts.size();
      children = context.pool.make_span<Query::Weight*>(n);
      sources = context.pool.make_span<Query::SegmentSource*>(n);
      // Disjunct scorers never see the collector floor, so they cannot prune.
      int32_t childFlags = (flags & ~(ALLOW_PRUNING | EXCLUSION_WINDOW_FILL)) | NEED_SCORES;
      for (size_t i = 0; i < n; i++) {
        children[i] = query.disjuncts[i]->createWeight(context, childFlags, multiplier);
        sources[i] = children[i];
      }
      if (QueryPrep::anyNeedsPrepare(children)) traits |= NEEDS_PREPARE;
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(
        Query::Weight::PrepareContext& ctx) override {
      return std::make_unique<Prepared>(QueryPrep::prepareSources(children, ctx), tieBreaker);
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      return buildPull(targetPool, scorerSupplier(targetPool, segment));
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      return makeSupplier(targetPool, segment, sources, tieBreaker, executionMode);
    }

    void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
      for (auto* child : children) child->releaseSegmentState(segment);
    }

    bool hasReleasableSegmentState() const noexcept override {
      for (auto* child : children) {
        if (child->hasReleasableSegmentState()) return true;
      }
      return false;
    }
  };

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    if ((flags & NEED_SCORES) == 0) {
#ifndef NDEBUG
      if (context.logicalValidationActive()) {
        membership->validateLogical(context.planningContext(), multiplier);
      }
#endif
      return membership->createWeight(context, flags, multiplier);
    }
    return context.pool.make<DisMaxQuery::Weight>(context, *this, flags, multiplier);
  }
};

static_assert(std::is_trivially_destructible_v<DisMaxQuery::Scorer>);

} // namespace luxir

#include "BooleanQuery.h"

namespace luxir {

inline Query* DisMaxQuery::make(MemPool& pool, std::span<Query*> disjuncts, float tieBreaker) {
  assert(!disjuncts.empty());
  if (disjuncts.size() == 1) return disjuncts[0];
  std::span<Query*> none{};
  auto* membership = pool.make<BooleanQuery>(none, disjuncts, none, none);
  // A full tie breaker adds every disjunct: that is the disjunction itself.
  if (tieBreaker == 1.0f) return membership;
  return pool.make<DisMaxQuery>(disjuncts, tieBreaker, membership);
}

} // namespace luxir
