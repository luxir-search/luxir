#pragma once

#include <algorithm>

#include "Query.h"
#include "DocSetBulkScorer.h"

namespace luxir {

class AllQuery final : public luxir::Query {
public:
  static inline bool disableDenseClauseForTests = false;

  AllQuery() {}

  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  bool canOmitWeightForCacheFirstMembership() const override { return true; }

  bool directCountAvailable(IndexReader& reader) const override {
    unused(reader);
    return true;
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::ALL);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  class Scorer final : public Query::ConstantScorer {
  public:
    luxir::IndexReader::Segment& segment;
    int32_t docid = -1;
    int32_t lastDoc;

    Scorer(luxir::IndexReader::Segment& segment,
           float constantScore = 0.0f)
      : Query::ConstantScorer(constantScore), segment(segment),
        lastDoc(segment.postingsReader().maxDoc() - 1) {
    }

    int32_t next() override {
      if (docid >= lastDoc) {
        docid = PostingsReader::END;
      } else {
        docid++;
      }
      return docid;
    }

    int32_t advance(int32_t target) override {
      assert(docId() < target);
      docid = target > lastDoc ? PostingsReader::END : target;
      return docid;
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docid;
    }

    void exhaust() override { lastDoc = -1; }

    void fillWindowBits(std::span<uint64_t> windowBits, int32_t windowStart,
                        int32_t windowEnd) override {
      skipCount(SkipStats::countBulkFillCalls);
      if (windowEnd <= windowStart
          || docid == PostingsReader::END || docid >= windowEnd) return;

      int32_t start = std::max(windowStart, docid < 0 ? windowStart : docid);
      int32_t end = std::min(windowEnd, lastDoc + 1);
      if (start < end) {
        int32_t first = start - windowStart;
        int32_t last = end - windowStart;
        int32_t firstWord = first >> 6;
        int32_t lastWord = (last - 1) >> 6;
        uint64_t firstMask = ~0ULL << (first & 63);
        uint64_t lastMask = ~0ULL >> (63 - ((last - 1) & 63));
        if (firstWord == lastWord) {
          windowBits[(size_t)firstWord] |= firstMask & lastMask;
        } else {
          windowBits[(size_t)firstWord] |= firstMask;
          std::fill(windowBits.begin() + firstWord + 1,
                    windowBits.begin() + lastWord, ~0ULL);
          windowBits[(size_t)lastWord] |= lastMask;
        }
      }
      docid = windowEnd <= lastDoc ? windowEnd : PostingsReader::END;
    }
  };

  class Supplier final : public Query::ScorerSupplier {
    luxir::IndexReader::Segment& segment;
    float score;
    // The test control is supplier-fixed so resolve and its later build cannot
    // observe different global values.
    bool denseClause;

    class Plan final : public Query::ScorerPlan {
      Supplier& supplier;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        return targetPool.make<AllQuery::Scorer>(
            supplier.segment, supplier.score);
      }

      Query::Scorer* buildIndependentScorer(
          MemPool& targetPool) override {
        return targetPool.make<AllQuery::Scorer>(
            supplier.segment, supplier.score);
      }

    public:
      Plan(Supplier& supplier, const Query::PlanContext& planContext,
           const Query::ScorerShape& shape, int64_t cost)
        : Query::ScorerPlan(planContext, shape, cost),
          supplier(supplier) {}
    };

  public:
    Supplier(luxir::IndexReader::Segment& segment, float score)
      : segment(segment), score(score),
        denseClause(!disableDenseClauseForTests) {}

    int64_t cost() override { return segment.maxDoc(); }

    Query::ScorerShape describeScorer(
        const Query::PlanContext& buildContext) const override {
      unused(buildContext);
      return {
        .matchState = Query::MatchState::NONEMPTY,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = Query::ReportedTwoPhase::NO,
        .windowFillClause = denseClause
            ? Query::ClauseShape::DIRECT
            : Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
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
      if (bulkContext.requireConstantCount) {
        if (segment.liveDocs() != nullptr) {
          return {
            BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
            BulkAnswer::NO,
          };
        }
        return {
          BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO,
          BulkAnswer::NO, nullptr, segment.maxDoc(),
        };
      }
      bool available = !bulkContext.requireFilterConsumption;
      return {
        available ? BulkAnswer::YES : BulkAnswer::NO,
        available ? BulkAnswer::YES : BulkAnswer::NO,
        BulkAnswer::NO,
        BulkAnswer::NO,
      };
    }

    // The null-source form of DocSetBulkScorer: all docs in [0, maxDoc), so
    // the per-call filter becomes the window source directly.
    BulkScorer* buildBulk(
        MemPool& targetPool, const BulkPlan& plan) override {
      assert(plan.available == BulkAnswer::YES);
      assert(!plan.hasConstantCount());
      return targetPool.make<DocSetBulkScorer>(
          targetPool, nullptr, segment.maxDoc(), score);
    }
  };

  class Weight final : public Query::Weight {
  protected:
    AllQuery& query;
    float score;
  public:
    Weight(Context& context, AllQuery& query, int32_t flags, float score)
      : Query::Weight(context, query, flags), query(query), score(score) {
      traits |= IS_CONSTANT_SCORING | MATCHES_ALL_DOCS;
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, luxir::IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(executionMode);
      return targetPool.make<AllQuery::Supplier>(segment, score);
    }

    AllQuery::Scorer* createScorer(luxir::MemPool& targetPool, luxir::IndexReader::Segment& segment) override {
      return targetPool.make<AllQuery::Scorer>(segment, score);
    }

    std::optional<int64_t> constantCount(
        IndexReader::Segment& segment, DocSet* domain) override {
      return domain == nullptr
          ? std::optional<int64_t>((int64_t) segment.maxDoc())
          : std::nullopt;
    }

  };

  AllQuery::Weight* createWeight(Context& context, int32_t flags,
                                 float multiplier = 1.0f) override {
    AllQuery::Weight* weight = context.pool.make<AllQuery::Weight>(
        context, *this, flags, constantWhenScored(flags, multiplier));
    return weight;
  }

};

} // namespace luxir
