// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <limits>
#include <span>
#include <string_view>

#include "AllQuery.h"
#include "Query.h"
#include "luxir/reader/DocsReader.h"

namespace luxir {

// Matches documents that supplied at least one accepted value for a field.
// Presence is read directly from SegFieldInfo, so indexed, column-only, and
// indexed text fields with no terms share the same execution path.
class ExistsQuery final : public Query {
  std::string_view field;

public:
  bool recoverableAsPredicate() const noexcept override { return true; }

  explicit ExistsQuery(std::string_view field)
    : Query(QueryKind::EXISTS), field(field) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const ExistsQuery&>(other);
    return field == rhs.field;
  }

  uint64_t hashImpl() const override {
    return mixHash(Query::hashImpl(), field);
  }

  std::string_view getField() const { return field; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  class Weight;

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override;

  class Scorer final : public Query::ConstantScorer {
    DocsReader docs;
    screaming::BitSet::Iterator iterator;

  public:
    Scorer(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo,
           float constantScore)
        : Query::ConstantScorer(constantScore),
          docs(postingsReader, fieldInfo), iterator(docs.bitset()) {
      assert(fieldInfo.docsWithField > 0);
      assert(fieldInfo.docsWithField < postingsReader.maxDoc());
      assert(docs.hasBitset());
    }

    [[gnu::cold]] void reset() override {
      iterator.reset();
    }

    int32_t next() override {
      return iterator.next();
    }

    int32_t advance(int32_t target) override {
      assert(docId() < target);
      return iterator.advance(target);
    }

    int32_t docId() override { return iterator.val(); }

    void fillScoredWindowBits(std::span<uint64_t> words,
                              int32_t min, int32_t max) override {
      fillWindowBits(words, min, max);
    }

    void fillWindowBits(std::span<uint64_t> words, int32_t min,
                        int32_t max) override {
      if (min >= max) return;
      int32_t doc = iterator.val();
      if (doc < min) doc = iterator.advance(min);
      while (doc < max) {
        int32_t bit = doc - min;
        words[(size_t)bit >> 6] |= 1ULL << (bit & 63);
        doc = iterator.next();
      }
    }
  };

  class Weight final : public Query::Weight {
    ExistsQuery& query;
    IndexReader::FieldSegments segInfos;
    float constantScore;

    const SegFieldInfo* segmentInfo(IndexReader::Segment& segment) const {
      return segInfos.at((size_t)segment.ord);
    }

  public:
    Weight(Context& context, ExistsQuery& query, int32_t flags,
           float constantScore)
        : Query::Weight(context, query, flags), query(query),
          segInfos(context.topReader.field(query.getField())),
          constantScore(constantScore) {
      traits |= IS_CONSTANT_SCORING;
    }

    // Header totals include deleted docs, so a restricted segment needs iteration.
    CountShortcut countShortcut(IndexReader::Segment& segment, DocSet* domain) override {
      const auto* info = segmentInfo(segment);
      return countBound(segment, domain, info ? info->docsWithField : 0,
                        CountHow::METADATA);
    }

    class Supplier final : public Query::ScorerSupplier {
      IndexReader::Segment& segment;
      const SegFieldInfo& fieldInfo;
      float constantScore;
      bool needsScores;

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          if (supplier.fieldInfo.docsWithField
              == supplier.segment.maxDoc()) {
            return targetPool.make<AllQuery::Scorer>(
                supplier.segment, supplier.constantScore);
          }
          return targetPool.make<ExistsQuery::Scorer>(
              supplier.segment.postingsReader(), supplier.fieldInfo,
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
      Supplier(IndexReader::Segment& segment, const SegFieldInfo& fieldInfo,
               float constantScore, bool needsScores)
          : segment(segment), fieldInfo(fieldInfo),
            constantScore(constantScore), needsScores(needsScores) {}

      int64_t cost() override { return fieldInfo.docsWithField; }

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
          .windowFillClause = fieldInfo.docsWithField == segment.maxDoc()
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
      const SegFieldInfo* info = segmentInfo(segment);
      if (info == nullptr || info->docsWithField == 0) return nullptr;
      return targetPool.make<Supplier>(segment, *info, constantScore, needsScores());
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

static_assert(std::is_trivially_destructible_v<ExistsQuery::Weight>);
static_assert(std::is_trivially_destructible_v<ExistsQuery::Scorer>);

} // namespace luxir
