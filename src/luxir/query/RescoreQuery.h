#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "luxir/query/Query.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/value/ValueExpr.h"

namespace luxir {

class RescoreQuery final : public Query {
  Query* child;
  ValueProgram* program;
  std::optional<float> constantOutput;

  static ValueBounds scoreEnvironment(ScoreBounds bounds) {
    if (!std::isfinite(bounds.lo) || !std::isfinite(bounds.hi)
        || bounds.lo > bounds.hi) {
      return ValueBounds::unbounded(ValueType::DOUBLE);
    }
    ValueBounds out = ValueBounds::floating((double)bounds.lo, (double)bounds.hi);
    out.minAttained = bounds.lo == bounds.hi;
    out.maxAttained = bounds.lo == bounds.hi;
    return out;
  }

  static ScoreBounds outputBounds(const ValueBounds& bounds, float multiplier) {
    if (bounds.certainty != BoundsCertainty::BOUNDED || bounds.mayBeMissing
        || bounds.alwaysMissing || valueArray(bounds.type)
        || bounds.type == ValueType::COLUMN_ONLY) {
      return ScoreBounds::unknown();
    }
    constexpr double MAX_FLOAT = (double)std::numeric_limits<float>::max();
    double low = 0.0;
    double high = 0.0;
    float outLow;
    float outHigh;
    if (bounds.type == ValueType::DOUBLE) {
      low = bounds.doubleMin;
      high = bounds.doubleMax;
      if (!std::isfinite(low) || !std::isfinite(high)
          || low < -MAX_FLOAT || high > MAX_FLOAT) {
        return ScoreBounds::unknown();
      }
      outLow = (float)low;
      outHigh = (float)high;
    } else {
      outLow = (float)bounds.intMin;
      outHigh = (float)bounds.intMax;
    }
    outLow *= multiplier;
    outHigh *= multiplier;
    if (!std::isfinite(outLow) || !std::isfinite(outHigh)) {
      return ScoreBounds::unknown();
    }
    if ((bounds.type == ValueType::DOUBLE && low == high)
        || (bounds.type == ValueType::INT64
            && bounds.intMin == bounds.intMax)) {
      return ScoreBounds::exact(outLow);
    }
    return {
        std::nextafter(outLow, -std::numeric_limits<float>::infinity()),
        std::nextafter(outHigh, std::numeric_limits<float>::infinity())};
  }

  class Scorer final : public Query::Scorer {
    Query::Scorer* child;
    BoundValueProgram* expression;
    float multiplier;
    float minCompetitiveScore = -std::numeric_limits<float>::infinity();
    ScoreBounds shallowBounds;
    int32_t doc = -1;
    int32_t shallowUpTo = -1;
    int32_t cachedDoc = -1;
    uint32_t cachedChildScoreBits = 0;
    float cachedOutput = 0.0f;
    bool needsChildScore;
    bool pruning;
    bool cacheValid = false;
    bool shallowBoundsValid = false;

    float convertResult(const ValueResult& result) const {
      if (!result.valid) {
        throw std::runtime_error(fmt::format(
            "rescore expression is missing for matched segment doc {}; use def() "
            "in the expression or exists() in the child query",
            doc));
      }
      constexpr double MAX_FLOAT = (double)std::numeric_limits<float>::max();
      float output;
      if (result.type == ValueType::DOUBLE) {
        double value = result.doubleValue;
        if (!std::isfinite(value) || value < -MAX_FLOAT || value > MAX_FLOAT) {
          throw std::runtime_error(fmt::format(
              "rescore expression result is outside the finite float score range "
              "at segment doc {}",
              doc));
        }
        output = (float)value;
      } else {
        output = (float)result.intValue;
      }
      output *= multiplier;
      if (!std::isfinite(output)) {
        throw std::runtime_error(fmt::format(
            "rescore output is outside the finite float score range at segment doc {}",
            doc));
      }
      return output;
    }

    ScoreBounds composeBounds(int32_t upTo, bool refined) {
      ScoreBounds childBounds = refined
          ? child->refineScoreBounds(upTo) : child->getScoreBounds(upTo);
      const ValueBounds& valueBounds =
          expression->boundsForScore(scoreEnvironment(childBounds));
      return outputBounds(valueBounds, multiplier);
    }

    int32_t skipUncompetitive(bool approximation, int32_t candidate) {
      while (candidate != PostingsReader::END && pruning
             && minCompetitiveScore != -std::numeric_limits<float>::infinity()) {
        if (candidate > shallowUpTo) {
          shallowUpTo = child->advanceShallow(candidate);
          shallowBounds = getScoreBounds(shallowUpTo);
          if (shallowBounds.hi >= minCompetitiveScore) {
            shallowBounds = refineScoreBounds(shallowUpTo);
            // Refinement may parse a coarse impact group. Re-asking shallow
            // lets the child expose the now-known block boundary, so the next
            // candidate does not retain an earlier block's maximum.
            shallowUpTo = child->advanceShallow(candidate);
            shallowBounds = getScoreBounds(shallowUpTo);
          }
          shallowBoundsValid = true;
        }
        assert(shallowBoundsValid);
        if (shallowBounds.hi >= minCompetitiveScore) {
          break;
        }
        if (shallowUpTo == PostingsReader::END) {
          candidate = PostingsReader::END;
          break;
        }
        int32_t target = shallowUpTo + 1;
        candidate = approximation
            ? child->approximationAdvance(target) : child->advance(target);
      }
      doc = candidate;
      cacheValid = false;
      return doc;
    }

  public:
    Scorer(Query::Scorer* child, BoundValueProgram* expression,
           float multiplier, bool needsChildScore, bool pruning)
      : child(child), expression(expression), multiplier(multiplier),
        needsChildScore(needsChildScore), pruning(pruning) {
      // Re-run env-sensitive validation against the child's actual global
      // interval as part of scorer binding, before any matching begins.
      composeBounds(PostingsReader::END, false);
    }

    int32_t next() override {
      return skipUncompetitive(false, child->next());
    }

    int32_t advance(int32_t target) override {
      assert(doc < target);
      return skipUncompetitive(false, child->advance(target));
    }

    int32_t docId() override { return doc; }


    int32_t approximationNext() override {
      return skipUncompetitive(true, child->approximationNext());
    }

    int32_t approximationAdvance(int32_t target) override {
      assert(doc < target);
      return skipUncompetitive(true, child->approximationAdvance(target));
    }

    int32_t approximationDocId() override { return doc; }

    std::span<DocsPosEnum*> approximationEnums() override {
      return child->approximationEnums();
    }

    bool matches() override {
      bool matched = child->matches();
      if (!matched) cacheValid = false;
      return matched;
    }

    bool matchesAt(int32_t target) override {
      doc = target;
      cacheValid = false;
      return child->matchesAt(target);
    }

    float matchCost() override { return child->matchCost(); }

    float score() override {
      if (cacheValid && cachedDoc == doc) {
        return cachedOutput;
      }
      float childScore = needsChildScore ? child->score() : 0.0f;
      uint32_t scoreBits = std::bit_cast<uint32_t>(childScore);
      if (!cacheValid || cachedDoc != doc
          || cachedChildScoreBits != scoreBits) {
        cachedOutput = convertResult(expression->evalPoint(doc, childScore));
        cachedDoc = doc;
        cachedChildScoreBits = scoreBits;
        cacheValid = true;
      }
      return cachedOutput;
    }

    void setMinCompetitiveScore(float minScore) override {
      minCompetitiveScore = std::max(minCompetitiveScore, minScore);
    }

    float getMaxScore(int32_t upTo) override {
      return getScoreBounds(upTo).hi;
    }

    float refineMaxScore(int32_t upTo) override {
      return refineScoreBounds(upTo).hi;
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return composeBounds(upTo, false);
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return composeBounds(upTo, true);
    }

    int32_t advanceShallow(int32_t target) override {
      shallowUpTo = -1;
      shallowBoundsValid = false;
      return child->advanceShallow(target);
    }

    std::string_view pruningBlockerForDebug() const override {
      std::string_view blocker = expression->firstUnboundedNode();
      if (!blocker.empty()) return blocker;
      ScoreBounds output =
          outputBounds(expression->bounds(expression->program.rootNode),
                       multiplier);
      return std::isfinite(output.lo) && std::isfinite(output.hi)
          ? std::string_view{} : expression->program.root().text;
    }
  };

  class Supplier final : public Query::ScorerSupplier {
    Query::ScorerSupplier* childSupplier;
    ValueProgram* program;
    IndexReader::Segment* segment;
    float multiplier;
    bool needsChildScore;
    bool pruning;

    class Plan final : public Query::ScorerPlan {
      Query::ScorerPlan* childPlan;
      ValueProgram* program;
      IndexReader::Segment* segment;
      float multiplier;
      bool needsChildScore;
      bool pruning;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        Query::Scorer* childScorer = childPlan->build(targetPool);
        if (childScorer == nullptr) return nullptr;
        BoundValueProgram* expression =
            program->bind(targetPool, *segment);
        const ValueBounds& root =
            expression->bounds(program->rootNode);
        if (root.alwaysMissing) {
          throw std::runtime_error(
              "rescore expression is always missing for a matchable "
              "segment; use def() in the expression or exists() in the "
              "child query");
        }
        return targetPool.make<Scorer>(
            childScorer, expression, multiplier, needsChildScore, pruning);
      }

    public:
      Plan(Supplier& supplier,
           const Query::PlanContext& planContext,
           const Query::ScorerShape& shape, int64_t cost,
           Query::ScorerPlan* childPlan, ValueProgram* program,
           IndexReader::Segment* segment, float multiplier,
           bool needsChildScore, bool pruning)
        : Query::ScorerPlan(
              planContext, shape, cost),
          childPlan(childPlan), program(program), segment(segment),
          multiplier(multiplier), needsChildScore(needsChildScore),
          pruning(pruning) {}
    };

  public:
    Supplier(Query::ScorerSupplier* childSupplier,
             ValueProgram* program, IndexReader::Segment* segment,
             float multiplier,
             bool needsChildScore, bool pruning)
      : childSupplier(childSupplier), program(program), segment(segment),
        multiplier(multiplier), needsChildScore(needsChildScore),
        pruning(pruning) {}

    int64_t cost() override { return childSupplier->cost(); }

    Query::ScorerShape describeScorer(
        const Query::PlanContext& buildContext) const override {
      Query::ScorerShape child = childSupplier->describeScorer(buildContext);
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
          *this, planContext, describeScorer(planContext),
          childPlan->cost(), childPlan, program, segment, multiplier,
          needsChildScore, pruning);
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

  static Query::ScorerSupplier* wrapSupplier(
      MemPool& targetPool, IndexReader::Segment& segment,
      Query::SegmentSource& childSource, ValueProgram& program,
      float multiplier, bool needsChildScore, bool pruning,
      Query::SupplierExecutionMode executionMode) {
    Query::ScorerSupplier* childSupplier =
        childSource.scorerSupplier(targetPool, segment, executionMode);
    if (childSupplier == nullptr) return nullptr;
    return targetPool.make<Supplier>(
        childSupplier, &program, &segment, multiplier, needsChildScore, pruning);
  }

public:
  RescoreQuery(Query* child, ValueProgram* program,
               std::optional<float> constantOutput = std::nullopt)
    : Query(QueryKind::RESCORE), child(child), program(program),
      constantOutput(constantOutput) {}

  bool equals(const Query& other) const override {
    unused(other);
    return false;
  }

  uint64_t hashImpl() const override { return Query::hashImpl(); }

  Query* getChild() const { return child; }
  ValueProgram& getProgram() const { return *program; }

  bool canOmitWeightForCacheFirstMembership() const override {
    return child->canOmitWeightForCacheFirstMembership();
  }

  bool directCountAvailable(IndexReader& reader) const override {
    return child->directCountAvailable(reader);
  }

  bool exactDomainIdentity() const override {
    return child->exactDomainIdentity();
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    if (!std::isfinite(multiplier) || multiplier < 0.0f) {
      throw std::runtime_error(
          "rescore multiplier must be finite and non-negative");
    }
    child->validateLogical(context, 1.0f);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    return child->appendFilterKey(out, ctx);
  }

  ScoreProfile scoreProfile() const override {
    return constantOutput.has_value()
        ? ScoreProfile::explicitUniform(*constantOutput)
        : ScoreProfile::variable();
  }

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override;

  class Weight final : public Query::Weight {
    Query::Weight* childWeight;
    ValueProgram* program;
    float multiplier;
    bool scored;
    bool needsChildScore;

    class Prepared final : public Query::Weight::PreparedWeight {
      QueryPrep::PreparedSource child;
      ValueProgram* program;
      float multiplier;
      bool scored;
      bool needsChildScore;
      bool pruning;

    public:
      Prepared(QueryPrep::PreparedSource&& child, ValueProgram* program,
               float multiplier, bool scored, bool needsChildScore,
               bool pruning)
        : child(std::move(child)), program(program), multiplier(multiplier),
          scored(scored), needsChildScore(needsChildScore), pruning(pruning) {}

      Query::Scorer* createScorer(
          MemPool& targetPool, IndexReader::Segment& segment) override {
        Query::ScorerSupplier* supplier = scorerSupplier(targetPool, segment);
        if (supplier == nullptr) return nullptr;
        Query::Demand demand = Query::Demand::fromLeadCost(
            std::numeric_limits<int64_t>::max());
        return supplier->resolve(
            targetPool, supplier->makePlanContext(demand))->build(targetPool);
      }

      Query::ScorerSupplier* scorerSupplierImpl(
          MemPool& targetPool, IndexReader::Segment& segment,
          Query::SupplierExecutionMode executionMode) override {
        Query::SegmentSource& source = child.segmentSource();
        if (!scored) {
          return source.scorerSupplier(targetPool, segment, executionMode);
        }
        return wrapSupplier(targetPool, segment, source, *program, multiplier,
                            needsChildScore, pruning, executionMode);
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
    Weight(Context& context, RescoreQuery& query, int32_t flags,
           float multiplier)
      : Query::Weight(context, query, flags), program(query.program),
        multiplier(multiplier), scored((flags & NEED_SCORES) != 0),
        needsChildScore(scored && program->needsScore) {
      int32_t childFlags = needsChildScore ? flags : flags & ~NEED_SCORES;
      childWeight = query.child->createWeight(context, childFlags, 1.0f);
      traits = childWeight->getFlags() & ~IS_CONSTANT_SCORING;
      if (!scored || query.constantOutput.has_value()) {
        traits |= IS_CONSTANT_SCORING;
      }
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(
        Query::Weight::PrepareContext& ctx) override {
      QueryPrep::PreparedSource source;
      source.weight = childWeight;
      if (childWeight->needsPrepare()) {
        source.setPrepared(childWeight->prepare(ctx));
      }
      return std::make_unique<Prepared>(
          std::move(source), program, multiplier, scored, needsChildScore,
          allowsPruning());
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      Query::ScorerSupplier* supplier = scorerSupplier(targetPool, segment);
      if (supplier == nullptr) return nullptr;
      Query::Demand demand = Query::Demand::fromLeadCost(
          std::numeric_limits<int64_t>::max());
      return supplier->resolve(
          targetPool, supplier->makePlanContext(demand))->build(targetPool);
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      if (!scored) {
        return childWeight->scorerSupplier(
            targetPool, segment, executionMode);
      }
      return wrapSupplier(targetPool, segment, *childWeight, *program,
                          multiplier, needsChildScore, allowsPruning(),
                          executionMode);
    }

    std::optional<int64_t> constantCount(
        IndexReader::Segment& segment, DocSet* domain) override {
      return childWeight->constantCount(segment, domain);
    }


    bool childNeedsScoresForTest() const { return childWeight->needsScores(); }
  };
};

inline Query::Weight* RescoreQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  return context.pool.make<RescoreQuery::Weight>(
      context, *this, flags, multiplier);
}

} // namespace luxir
