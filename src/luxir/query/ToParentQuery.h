// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "MinScoreQuery.h"
#include "Query.h"
#include "QueryPrep.h"
#include "luxir/reader/OwnerMap.h"

namespace luxir {

/// Children to owner: matches the owner rows of the elements of nested `path`
/// that match `child`. The child query is planned and executed in the path's
/// own row space (IndexReader::scope), so every clause of it evaluates on one
/// element. Element rows are stored in owner order, so child matches arrive
/// grouped by owner and the scorer reduces each run onto its owner as it
/// streams: no structure sized by rows or owners is built per request.
///
/// Scores: max (default), min, sum, avg of the matching elements' scores, or
/// none (0). min_children / max_children bound the number of matching
/// elements. With max and no count bound, the collector floor passes down to
/// the child: an owner cannot beat its best element.
///
/// Liveness: scorers do not filter deletes. Elements of a deleted owner are
/// dead with it, and its (deleted) owner row is dropped by the consumer's
/// domain like any other deleted row.
class ToParentQuery final : public Query {
public:
  enum class ScoreMode : uint8_t { MAX, MIN, SUM, AVG, NONE };

private:
  std::string_view path;
  std::string_view name;
  Query* child;
  ScoreMode mode;
  int32_t minChildren;
  int32_t maxChildren;

  bool countBounded() const { return minChildren > 1 || maxChildren < std::numeric_limits<int32_t>::max(); }

  class Scorer final : public Query::Scorer {
    Query::Scorer* child;
    const OwnerMap& owners;
    OwnerMap::OwnerCursor ownerOf;
    ScoreMode mode;
    int32_t minChildren;
    int32_t maxChildren;
    float multiplier;
    bool scored;
    bool prune;
    bool exhausted = false;  // a constant score fell below the collector floor
    int32_t doc = -1;
    int32_t pending = -1;  // child's current doc not yet reduced (-1: not started)
    float current = 0.0f;
    std::optional<float> constant;  // the score of every match, when uniform
    ScoreBounds bounds;

    // The owner score as reduce() computes it from an element score `s`
    // shared by all of the owner's matching elements.
    float ownerScore(float s) const { return (float)((double)s * multiplier); }

    // max, min and avg of an owner's matching element scores lie within the
    // element bounds, so they bound the owner's score too (sum does not).
    void initBounds() {
      if (!scored) {
        constant = 0.0f;
        bounds = ScoreBounds::exact(0.0f);
        return;
      }
      if (mode == ScoreMode::SUM || !(multiplier >= 0.0f)) return;
      if (auto c = child->constantContribution()) {
        constant = ownerScore(*c);
        bounds = ScoreBounds::exact(*constant);
        return;
      }
      ScoreBounds b = child->getScoreBounds(PostingsReader::END);
      bounds.hi = std::min(b.hi, child->getMaxScore(PostingsReader::END));
      bounds.lo = b.lo;
      if (std::isfinite(bounds.hi)) bounds.hi = ownerScore(bounds.hi);
      if (std::isfinite(bounds.lo)) bounds.lo = ownerScore(bounds.lo);
    }

    int32_t reduce(int32_t c) {
      while (c != PostingsReader::END) {
        int32_t owner = ownerOf.ownerOf(c);
        int32_t count = 0;
        double acc = mode == ScoreMode::MIN ? std::numeric_limits<double>::infinity()
                   : mode == ScoreMode::MAX ? -std::numeric_limits<double>::infinity() : 0.0;
        int32_t end = owners.begin(owner + 1);
        while (c != PostingsReader::END && c < end) {
          if (scored) {
            float s = child->score();
            switch (mode) {
              case ScoreMode::MAX: acc = std::max(acc, (double)s); break;
              case ScoreMode::MIN: acc = std::min(acc, (double)s); break;
              case ScoreMode::SUM: case ScoreMode::AVG: acc += s; break;
              case ScoreMode::NONE: break;
            }
          }
          count++;
          c = child->next();
        }
        pending = c;
        if (count >= minChildren && count <= maxChildren) {
          double value = 0.0;
          if (scored) {
            value = mode == ScoreMode::AVG ? acc / count : mode == ScoreMode::NONE ? 0.0 : acc;
          }
          current = (float)(value * multiplier);
          return doc = owner;
        }
      }
      pending = PostingsReader::END;
      return doc = PostingsReader::END;
    }

  public:
    Scorer(Query::Scorer* child, const OwnerMap& owners, ScoreMode mode, int32_t minChildren,
           int32_t maxChildren, float multiplier, bool scored, bool prune)
      : child(child), owners(owners), ownerOf(owners), mode(mode), minChildren(minChildren),
        maxChildren(maxChildren), multiplier(multiplier), scored(scored && mode != ScoreMode::NONE),
        prune(prune) {
      initBounds();
    }

    int32_t next() override {
      if (pending == PostingsReader::END || exhausted) return doc = PostingsReader::END;
      return reduce(pending == -1 ? child->next() : pending);
    }

    int32_t advance(int32_t target) override {
      assert(doc < target);
      if (pending == PostingsReader::END || exhausted) return doc = PostingsReader::END;
      int32_t begin = owners.begin(target);
      int32_t c = pending;
      if (c < begin) c = begin == 0 ? child->next() : child->advance(begin);
      return reduce(c);
    }

    int32_t docId() override { return doc; }

    float score() override { return current; }

    std::optional<float> constantContribution() const override { return constant; }
    float getMaxScore(int32_t upTo) override { unused(upTo); return bounds.hi; }
    float getMaxScoreForSetup(int32_t upTo) override { unused(upTo); return bounds.hi; }
    ScoreBounds getScoreBounds(int32_t upTo) override { unused(upTo); return bounds; }
    int32_t advanceShallowForSetup(int32_t target) override {
      unused(target);
      return PostingsReader::END;
    }

    void setMinCompetitiveScore(float floor) override {
      // Ties at the floor stay competitive; above a uniform score nothing
      // left can compete, so iteration ends (the current doc stays valid).
      if (constant && floor > *constant) exhausted = true;
      if (!prune || !scored || multiplier <= 0.0f) return;
      child->setMinCompetitiveScore(
          MinScoreQuery::childFloor(floor, -std::numeric_limits<float>::infinity(), multiplier, true));
    }
  };

  class Supplier final : public Query::ScorerSupplier {
    Query::ScorerSupplier* childSupplier;
    const OwnerMap& owners;
    ScoreMode mode;
    int32_t minChildren;
    int32_t maxChildren;
    float multiplier;
    bool scored;
    bool prune;

    class Plan final : public Query::ScorerPlan {
      Query::ScorerPlan* childPlan;
      const Supplier& supplier;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        Query::Scorer* childScorer = childPlan->build(targetPool);
        if (childScorer == nullptr) return nullptr;
        return targetPool.make<Scorer>(childScorer, supplier.owners, supplier.mode, supplier.minChildren,
                                       supplier.maxChildren, supplier.multiplier, supplier.scored,
                                       supplier.prune);
      }

    public:
      Plan(const Query::PlanContext& planContext, const Query::ScorerShape& shape, int64_t cost,
           Query::ScorerPlan* childPlan, const Supplier& supplier)
        : Query::ScorerPlan(planContext, shape, cost), childPlan(childPlan), supplier(supplier) {}
    };

  public:
    Supplier(Query::ScorerSupplier* childSupplier, const OwnerMap& owners, ScoreMode mode,
             int32_t minChildren, int32_t maxChildren, float multiplier, bool scored, bool prune)
      : childSupplier(childSupplier), owners(owners), mode(mode), minChildren(minChildren),
        maxChildren(maxChildren), multiplier(multiplier), scored(scored), prune(prune) {}

    // Owners never outnumber their matching elements.
    int64_t cost() override { return childSupplier->cost(); }

    Query::ScorerShape describeScorer(const Query::PlanContext& buildContext) const override {
      unused(buildContext);
      return {
        .matchState = Query::MatchState::UNKNOWN,
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

    Query::VerificationWork verificationWork(const Query::PlanContext& buildContext) const override {
      unused(buildContext);
      return Query::VerificationWork::PRESENT;
    }

    Query::ScorerPlan* resolve(MemPool& planPool, const Query::PlanContext& planContext) override {
      // The reduction pulls every matching element of an owner in order.
      Query::PlanContext childContext = childSupplier->makePlanContext(
          Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max()));
      childContext.demand.horizon = Query::ExecutionUse::PULL;
      Query::ScorerPlan* childPlan = childSupplier->resolve(planPool, childContext);
      assert(childPlan != nullptr);
      return planPool.make<Plan>(planContext, describeScorer(planContext), childPlan->cost(), childPlan,
                                 *this);
    }
  };

public:
  ToParentQuery(std::string_view path, std::string_view name, Query* child, ScoreMode mode,
                int32_t minChildren, int32_t maxChildren)
    : Query(QueryKind::TO_PARENT), path(path), name(name), child(child), mode(mode),
      minChildren(minChildren), maxChildren(maxChildren) {}

  std::string_view getPath() const { return path; }
  std::string_view getName() const { return name; }
  Query* getChild() const { return child; }
  ScoreMode getMode() const { return mode; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const ToParentQuery&>(other);
    return path == rhs.path && mode == rhs.mode && minChildren == rhs.minChildren
        && maxChildren == rhs.maxChildren && child->equals(*rhs.child);
  }

  uint64_t hashImpl() const override {
    uint64_t h = mixHash(Query::hashImpl(), std::hash<std::string_view>{}(path));
    h = mixHash(h, (uint64_t)mode);
    h = mixHash(h, (uint64_t)(uint32_t)minChildren);
    h = mixHash(h, (uint64_t)(uint32_t)maxChildren);
    return mixHash(h, child->hash());
  }

  void validateLogicalImpl(PlanningContext& context, float multiplier = 1.0f) const override {
    checkedBoostProduct(multiplier, 1.0f);
    child->validateLogical(context.scope(path), 1.0f);
  }

  // Owner membership depends on the child's membership and the count bounds,
  // not on scores, so it is keyed in the owner space by path and child key.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext& ctx) const override {
    out.appendKind(QueryKind::TO_PARENT);
    out.appendString(path);
    out.appendInt32(minChildren);
    out.appendInt32(maxChildren);
    return child->appendFilterKey(out, ctx);
  }

  Weight* createWeight(Context& context, int32_t flags, float multiplier = 1.0f) override;

  // Child terms are element fields; owner-space visitors (highlighting) must
  // not see them as owner fields.
  void visitImpl(QueryVisitor& visitor, Context& context, float boost) const override;

  class Weight final : public Query::Weight {
    ToParentQuery& query;
    Query::Weight* childWeight;
    IndexReader& scopeReader;
    float multiplier;
    bool scored;
    bool prune;

    Query::ScorerSupplier* wrap(MemPool& targetPool, IndexReader::Segment& segment,
                                Query::SegmentSource& source, Query::SupplierExecutionMode executionMode) {
      auto& childSegment = scopeReader.segments()[(size_t)segment.ord];
      if (childSegment.maxDoc() == 0) return nullptr;
      Query::ScorerSupplier* childSupplier = source.scorerSupplier(targetPool, childSegment, executionMode);
      if (childSupplier == nullptr) return nullptr;
      return targetPool.make<Supplier>(childSupplier, childSegment.postingsReader().owners(), query.mode,
                                       query.minChildren, query.maxChildren, multiplier, scored, prune);
    }

    static Query::Scorer* buildPull(MemPool& targetPool, Query::ScorerSupplier* supplier) {
      if (supplier == nullptr) return nullptr;
      Query::Demand demand = Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max());
      return supplier->resolve(targetPool, supplier->makePlanContext(demand))->build(targetPool);
    }

    class Prepared final : public Query::Weight::PreparedWeight {
      Weight& weight;
      QueryPrep::PreparedSource child;
      std::vector<std::shared_ptr<LiveDocs>> domains;  // descended owner domains, kept alive

    public:
      Prepared(Weight& weight, QueryPrep::PreparedSource&& child, std::vector<std::shared_ptr<LiveDocs>>&& domains)
        : weight(weight), child(std::move(child)), domains(std::move(domains)) {}

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        return buildPull(targetPool, scorerSupplier(targetPool, segment));
      }

      Query::ScorerSupplier* scorerSupplierImpl(MemPool& targetPool, IndexReader::Segment& segment,
                                                Query::SupplierExecutionMode executionMode) override {
        return weight.wrap(targetPool, segment, child.segmentSource(), executionMode);
      }

      void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
        child.releaseSegmentState(weight.scopeReader.segments()[(size_t)segment.ord]);
      }

      PreparedDomainDependence domainDependence() const noexcept override { return child.domainDependence; }
    };

  public:
    Weight(Context& context, ToParentQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, query, flags), query(query),
        scopeReader(context.scope(query.path).topReader), multiplier(multiplier),
        scored((flags & NEED_SCORES) != 0 && query.mode != ScoreMode::NONE) {
      prune = scored && query.mode == ScoreMode::MAX && !query.countBounded()
           && (flags & ALLOW_PRUNING) != 0;
      int32_t childFlags = flags & ~EXCLUSION_WINDOW_FILL;
      if (!scored) childFlags &= ~(NEED_SCORES | ALLOW_PRUNING);
      if (!prune) childFlags &= ~ALLOW_PRUNING;
      childWeight = query.child->createWeight(context.scope(query.path), childFlags, 1.0f);
      traits = childWeight->getFlags() & (NEEDS_PREPARE | CONSUMES_DOMAIN);
      // max, min and avg of one uniform element score is that score.
      if (!scored || (query.mode != ScoreMode::SUM && childWeight->isConstantScoring())) {
        traits |= IS_CONSTANT_SCORING;
      }
    }

    // The owner domain descends to the element rows of its owners: the
    // child prepares (kNN candidate admission, ...) over live elements of
    // eligible owners only.
    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override;

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      return buildPull(targetPool, scorerSupplier(targetPool, segment));
    }

    Query::ScorerSupplier* scorerSupplierImpl(MemPool& targetPool, IndexReader::Segment& segment,
                                              Query::SupplierExecutionMode executionMode) override {
      return wrap(targetPool, segment, *childWeight, executionMode);
    }

    void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
      childWeight->releaseSegmentState(scopeReader.segments()[(size_t)segment.ord]);
    }

    bool hasReleasableSegmentState() const noexcept override {
      return childWeight->hasReleasableSegmentState();
    }
  };
};

// Owner rows -> element rows: the live elements of the owners in `owners`
// (null = every owner). Returns null when the result is every live element of
// the segment, keeping the domain identity contract (a null domain stays null).
std::shared_ptr<LiveDocs> descendDomain(const IndexReader::Segment& ownerSegment, DocSet* owners,
                                        const IndexReader::Segment& scopeSegment);

} // namespace luxir
