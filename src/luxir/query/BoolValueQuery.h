// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

#include "AllQuery.h"
#include "BitSetScorer.h"
#include "Query.h"

namespace luxir {

// Matches the documents of a BOOLEAN column with the given value: the
// segment's true-docs or false-docs bitset (FieldType::BOOL_BITSETS), read
// straight from SegFieldInfo. QueryBuilder lowers a boolean match, range or
// any_of to this, to ExistsQuery (both values) or to match-nothing.
class BoolValueQuery final : public Query {
  std::string_view field;
  bool value;

public:
  bool recoverableAsPredicate() const noexcept override { return true; }

  BoolValueQuery(std::string_view field, bool value)
    : Query(QueryKind::BOOL_VALUE), field(field), value(value) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const BoolValueQuery&>(other);
    return field == rhs.field && value == rhs.value;
  }

  uint64_t hashImpl() const override {
    return mixHash(mixHash(Query::hashImpl(), field), (int64_t)value);
  }

  std::string_view getField() const { return field; }
  bool getValue() const { return value; }

  // Every match holds the value (0 or 1) in the column.
  std::optional<ValueEnvelope> requiredValueEnvelope(
      std::string_view target) const override {
    if (target != field) return std::nullopt;
    return ValueEnvelope{value ? 1 : 0, value ? 1 : 0};
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendByte(value ? 1 : 0);
    out.appendString(field);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  class Weight;

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override;

  class Weight final : public Query::Weight {
    BoolValueQuery& query;
    IndexReader::FieldSegments segInfos;
    float constantScore;

    // The segment field, with the set's docs count and end location.
    struct SegmentSet {
      const SegFieldInfo* info = nullptr;
      int32_t docs = 0;
      seg_location endLoc;
    };

    SegmentSet segmentSet(IndexReader::Segment& segment) const {
      const SegFieldInfo* info = segInfos.at((size_t)segment.ord);
      if (info == nullptr) return {};
      if ((info->flags & FieldType::BOOL_BITSETS) == 0) {
        throw std::runtime_error(std::format(
            "boolean field '{}' has no value bitsets in segment {}",
            query.getField(), segment.ord));
      }
      if (query.getValue()) return {info, info->trueDocs, info->trueDocsEndLoc};
      return {info, info->falseDocs, info->falseDocsEndLoc};
    }

  public:
    Weight(Context& context, BoolValueQuery& query, int32_t flags,
           float constantScore)
        : Query::Weight(context, query, flags), query(query),
          segInfos(context.topReader.field(query.getField())),
          constantScore(constantScore) {
      traits |= IS_CONSTANT_SCORING;
    }

    // The set's count includes deleted docs, so a restricted segment needs iteration.
    CountShortcut countShortcut(IndexReader::Segment& segment, DocSet* domain) override {
      return countBound(segment, domain, segmentSet(segment).docs, CountHow::METADATA);
    }

    class Supplier final : public Query::ScorerSupplier {
      IndexReader::Segment& segment;
      SegmentSet set;
      float constantScore;
      bool needsScores;

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          if (supplier.set.docs == supplier.segment.maxDoc()) {
            return targetPool.make<AllQuery::Scorer>(
                supplier.segment, supplier.constantScore);
          }
          return targetPool.make<BitSetScorer>(
              supplier.segment.postingsReader().getInputStreamSeek(supplier.set.endLoc).ptr(),
              supplier.constantScore);
        }

      public:
        [[gnu::cold]] bool supportsReset() const noexcept override {
          return unscoredResetUse() && !supplier.needsScores;
        }

        Plan(Supplier& supplier,
             const Query::PlanContext& planContext,
             const Query::ScorerShape& shape, int64_t cost)
          : Query::ScorerPlan(planContext, shape, cost),
            supplier(supplier) {}
      };

    public:
      Supplier(IndexReader::Segment& segment, SegmentSet set,
               float constantScore, bool needsScores)
          : segment(segment), set(set),
            constantScore(constantScore), needsScores(needsScores) {}

      int64_t cost() override { return set.docs; }

      MembershipWork membershipWork(int32_t maxDoc) override {
        unused(maxDoc);
        double matches = (double)cost();
        return {matches, 4 * matches, 0, 32, 64};
      }

      Query::ScorerShape describeScorer(
          const Query::PlanContext& buildContext) const override {
        unused(buildContext);
        return {
          .matchState = Query::MatchState::NONEMPTY,
          .directKind = Query::DirectScorerKind::OTHER,
          .reportedTwoPhase = Query::ReportedTwoPhase::NO,
          .windowFillClause = set.docs == segment.maxDoc()
              ? Query::ClauseShape::DIRECT
              : Query::ClauseShape::NONE,
          .termDisjunctionClause = Query::ClauseShape::NONE,
          .termConjunctionClause = Query::ClauseShape::NONE,
          .independentTerm =
              Query::IndependentTermAccess::UNSUPPORTED,
          .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
          .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        };
      }

      Query::ScorerPlan* resolve(
          MemPool& planPool,
          const Query::PlanContext& planContext) override {
        return planPool.make<Plan>(
            *this, planContext, describeScorer(planContext), cost());
      }

      BulkPlan planBulk(
          BulkUse use, const BulkScorerContext& bulkContext) override {
        unused(use);
        if (bulkContext.requireConstantCount
            && segment.liveDocs() == nullptr) {
          return {
            BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO,
            BulkAnswer::NO, nullptr, cost(),
          };
        }
        return {
          BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
          BulkAnswer::NO,
        };
      }
    };

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(executionMode);
      SegmentSet set = segmentSet(segment);
      if (set.info == nullptr || set.docs == 0) return nullptr;
      return targetPool.make<Supplier>(segment, set, constantScore, needsScores());
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
  };
};

static_assert(std::is_trivially_destructible_v<BoolValueQuery::Weight>);

} // namespace luxir
