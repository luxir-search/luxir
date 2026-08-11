#pragma once

#include <algorithm>

#include "Query.h"
#include "DocSetBulkScorer.h"

namespace solux {

class AllQuery final : public solux::Query {
public:
  static inline bool disableDenseClauseForTests = false;

  AllQuery() {}

  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::ALL);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  class Scorer final : public Query::ConstantScorer {
  public:
    solux::IndexReader::Segment& segment;
    int32_t docid = -1;
    int32_t lastDoc;

    Scorer(solux::IndexReader::Segment& segment, float constantScore = 0.0f)
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

    bool supportsWindowFilter() const override {
      return !disableDenseClauseForTests;
    }

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
    solux::IndexReader::Segment& segment;
    float score;
  public:
    Supplier(solux::IndexReader::Segment& segment, float score)
      : segment(segment), score(score) {}

    int64_t cost() override { return segment.maxDoc(); }

    Query::ScorerShape describeScorer(
        const Query::ScorerBuildContext& buildContext) const override {
      unused(buildContext);
      return {
        .matchState = Query::MatchState::NONEMPTY,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = Query::ReportedTwoPhase::NO,
        .windowFillClause = disableDenseClauseForTests
            ? Query::ClauseShape::NONE
            : Query::ClauseShape::DIRECT,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
      };
    }

    AllQuery::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
      unused(leadCost);
      return targetPool.make<AllQuery::Scorer>(segment, score);
    }

    Query::Scorer* getIndependent(MemPool& targetPool,
                                  int64_t leadCost) override {
      return get(targetPool, leadCost);
    }

    // The null-source form of DocSetBulkScorer: all docs in [0, maxDoc), so
    // the per-call filter becomes the window source directly.
    BulkScorer* bulkScorer(MemPool& targetPool) override {
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
      : Query::Weight(context, flags), query(query), score(score) {
      traits |= IS_CONSTANT_SCORING | MATCHES_ALL_DOCS;
    }

    Query::ScorerSupplier* scorerSupplier(
        MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      return targetPool.make<AllQuery::Supplier>(segment, score);
    }

    AllQuery::Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      return targetPool.make<AllQuery::Scorer>(segment, score);
    }

  };

  AllQuery::Weight* createWeight(Context& context, int32_t flags,
                                 float multiplier = 1.0f) override {
    AllQuery::Weight* weight = context.pool.make<AllQuery::Weight>(
        context, *this, flags, constantWhenScored(flags, multiplier));
    return weight;
  }

};

} // namespace solux
