#pragma once

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "PointsMaterialize.h"
#include "Query.h"
#include "luxir/reader/StrColReader.h"

namespace luxir {

// Exact membership over a raw column-only STRING field. Indexed term fields
// use TermInSetQuery instead; this path exists because the current column-only
// string format stores raw bytes rather than term ordinals.
class StringColumnInSetQuery final : public Query {
  std::string_view field;
  std::span<const std::string_view> values;

public:
  StringColumnInSetQuery(std::string_view field,
                         std::span<const std::string_view> values)
    : field(field), values(values) {}

  std::string_view getField() const { return field; }
  std::span<const std::string_view> getValues() const { return values; }

  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  bool canOmitWeightForCacheFirstMembership() const override { return true; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::IN);
    out.appendString(field);
    out.appendSize(values.size());
    for (std::string_view value : values) out.appendString(value);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    return context.pool.make<Weight>(
        context, *this, flags, constantWhenScored(flags, multiplier));
  }

  class Weight final : public Query::Weight {
    StringColumnInSetQuery& query;
    std::span<SegFieldInfo*> segInfos;
    float constantScore;

    SegFieldInfo* segmentInfo(IndexReader::Segment& segment) const {
      return segInfos.empty() ? nullptr : segInfos[(size_t)segment.ord];
    }

  public:
    Weight(Context& context, StringColumnInSetQuery& query, int32_t flags,
           float constantScore)
      : Query::Weight(context, query, flags), query(query),
        segInfos(context.readSegInfos(query.getField())),
        constantScore(constantScore) {
      traits |= IS_CONSTANT_SCORING;
    }

    class Supplier final : public Query::ScorerSupplier {
      struct Facts {
        PointsMaterialize::Materialized docs;
        int64_t count;
      };

      Weight& weight;
      MemPool& scratchPool;
      IndexReader::Segment& segment;
      StrColReader reader;
      Facts* facts = nullptr;

      bool contains(std::string_view value) const {
        auto selected = weight.query.getValues();
        return std::binary_search(selected.begin(), selected.end(), value);
      }

      bool matches(StrColReader::Iterator& iter,
                   StrColReader::DocValues& docValues) const {
        if (!reader.isMultiValued()) return contains(iter.value());
        auto [start, end] = iter.valueRange();
        for (int64_t rank = start; rank < end; rank++) {
          if (contains(docValues.valueAt(rank))) return true;
        }
        return false;
      }

      Facts& materialize(MemPool& pool) {
        if (facts != nullptr) return *facts;

        std::vector<int32_t> matched;
        matched.reserve((size_t)reader.docsWithValue());
        StrColReader::Iterator iter(reader);
        StrColReader::DocValues docValues(reader);
        for (int32_t doc = iter.next(); doc != StrColReader::Iterator::ENDDOC;
             doc = iter.next()) {
          if (matches(iter, docValues)) matched.push_back(doc);
        }

        PointsMaterialize::Materialized result;
        if (PointsMaterialize::useBitset(matched.size(), segment.maxDoc())) {
          auto words = pool.make_span<uint64_t>(
              FixedBitSet::sizeInWords(segment.maxDoc()));
          std::fill(words.begin(), words.end(), 0);
          FixedBitSet bits(words.data(), segment.maxDoc());
          for (int32_t doc : matched) bits.set(doc);
          result.words = words.data();
        } else {
          result.docs = pool.copy_span(std::span<int32_t>(matched));
        }
        facts = pool.make<Facts>(Facts{result, (int64_t)matched.size()});
        return *facts;
      }

      Query::ScorerShape shape() const {
        return {
          .matchState = facts == nullptr
              ? Query::MatchState::UNKNOWN
              : facts->count == 0 ? Query::MatchState::EMPTY
                                  : Query::MatchState::NONEMPTY,
          .directKind = Query::DirectScorerKind::OTHER,
          .reportedTwoPhase = Query::ReportedTwoPhase::NO,
          .windowFillClause = Query::ClauseShape::DIRECT,
          .termDisjunctionClause = Query::ClauseShape::NONE,
          .termConjunctionClause = Query::ClauseShape::NONE,
          .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
          .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
          .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        };
      }

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;
        const Facts& facts;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          if (facts.count == 0) return nullptr;
          return PointsMaterialize::scorerFor(
              targetPool, facts.docs, supplier.segment.maxDoc(),
              supplier.weight.constantScore);
        }

      public:
        Plan(Supplier& supplier, const Query::PlanContext& planContext,
             const Query::ScorerShape& shape, const Facts& facts)
          : Query::ScorerPlan(planContext, shape, facts.count),
            supplier(supplier), facts(facts) {}
      };

    public:
      Supplier(Weight& weight, MemPool& scratchPool,
               IndexReader::Segment& segment, const SegFieldInfo& fieldInfo)
        : weight(weight), scratchPool(scratchPool), segment(segment),
          reader(segment.postingsReader(), fieldInfo) {}

      int64_t cost() override {
        return facts == nullptr ? reader.docsWithValue() : facts->count;
      }

      Query::ScorerShape describeScorer(
          const Query::PlanContext& buildContext) const override {
        unused(buildContext);
        return shape();
      }

      Query::UnresolvedSupplierCause unresolvedScorerCause(
          const Query::PlanContext& buildContext) const override {
        unused(buildContext);
        return facts == nullptr
            ? Query::UnresolvedSupplierCause::OTHER
            : Query::UnresolvedSupplierCause::NONE;
      }

      bool fillExpansionMemo(
          const Query::PlanContext& buildContext) override {
        unused(buildContext);
        if (facts != nullptr) return false;
        materialize(scratchPool);
        return true;
      }

      Query::ScorerPlan* resolve(
          MemPool& planPool,
          const Query::PlanContext& planContext) override {
        const Facts& resolved = materialize(planPool);
        return planPool.make<Plan>(*this, planContext, shape(), resolved);
      }
    };

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(executionMode);
      SegFieldInfo* info = segmentInfo(segment);
      if (info == nullptr || info->docsWithField == 0) return nullptr;
      return targetPool.make<Supplier>(*this, targetPool, segment, *info);
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

static_assert(std::is_trivially_destructible_v<StringColumnInSetQuery::Weight>);

} // namespace luxir
