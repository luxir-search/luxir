// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "luxir/query/BooleanWeight.h"
#include "luxir/query/DisMaxQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/Collector.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// Several bulk windows, so theta is known before the later ones.
constexpr int32_t kDocs = 5 * DocsEnumMeta::L1_DOCS;

// Fixed per-doc score over an explicit doc list. When `prunes` it declares
// prunesOnMinCompetitiveScore, records every floor it is given and skips
// docs scoring below the current floor.
struct FloorClause {
  std::vector<int32_t> docs;
  std::vector<float> scores;
  std::vector<float> floors;
  float maxScore = 0.0f;
  bool prunes = true;
};

class FloorQuery final : public Query {
  FloorClause& clause;

  class FloorScorer final : public Query::Scorer {
    FloorClause& clause;
    size_t at = 0;
    bool started = false;
    float floor = 0.0f;

    int32_t settle() {
      while (at < clause.docs.size() && clause.scores[at] < floor) at++;
      return docId();
    }

  public:
    explicit FloorScorer(FloorClause& clause) : clause(clause) {}
    int32_t next() override {
      if (started) at++;
      started = true;
      return settle();
    }
    int32_t docId() override {
      if (!started) return -1;
      return at < clause.docs.size() ? clause.docs[at] : PostingsReader::END;
    }
    float score() override { return clause.scores[at]; }
    bool prunesOnMinCompetitiveScore() const override { return clause.prunes; }
    void setMinCompetitiveScore(float minScore) override {
      if (!clause.prunes) return;
      clause.floors.push_back(minScore);
      floor = std::max(floor, minScore);
    }
    float getMaxScore(int32_t) override { return clause.maxScore; }
    float getMaxScoreForSetup(int32_t) override { return clause.maxScore; }
  };

  class Supplier final : public Query::ScorerSupplier {
    FloorClause& clause;

    class Plan final : public Query::ScorerPlan {
      FloorClause& clause;

    protected:
      Query::Scorer* buildScorer(MemPool& pool) override {
        return pool.make<FloorScorer>(clause);
      }

    public:
      Plan(FloorClause& clause, const Query::PlanContext& context,
           const Query::ScorerShape& shape, int64_t cost)
        : Query::ScorerPlan(context, shape, cost), clause(clause) {}
    };

  public:
    explicit Supplier(FloorClause& clause) : clause(clause) {}
    int64_t cost() override { return (int64_t)clause.docs.size(); }
    Query::ScorerShape describeScorer(
        const Query::PlanContext&) const override {
      return {
        .matchState = Query::MatchState::NONEMPTY,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = Query::ReportedTwoPhase::NO,
        .windowFillClause = Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        .verificationPoints = std::nullopt,
      };
    }
    Query::ScorerPlan* resolve(MemPool& pool,
                               const Query::PlanContext& context) override {
      Query::ScorerShape shape = describeScorer(context);
      return pool.make<Plan>(clause, context, shape, cost());
    }
  };

  class Weight final : public Query::Weight {
    FloorClause& clause;

  public:
    Weight(Query::Context& context, const Query& query, int32_t flags,
           FloorClause& clause)
      : Query::Weight(context, query, flags), clause(clause) {}
    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& pool, IndexReader::Segment&,
        Query::SupplierExecutionMode) override {
      return pool.make<Supplier>(clause);
    }
    Query::Scorer* createScorer(MemPool& pool, IndexReader::Segment&) override {
      return pool.make<FloorScorer>(clause);
    }
  };

public:
  explicit FloorQuery(FloorClause& clause)
    : Query(QueryKind::TEST), clause(clause) {}
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext&) const override {
    out.appendKind(kind);
    return FilterKeyScope::UNCACHEABLE;
  }
  Query::Weight* createWeight(Query::Context& context, int32_t flags,
                              float = 1.0f) override {
    return context.pool.make<Weight>(context, *this, flags, clause);
  }
};

using Hits = std::vector<TopDocsCollector::DocScore>;

class ClauseFloorTest : public LuxirTest {
public:
  CollectionHelper helper;
  FloorClause clause;
  bool bulkMissing = false;
  decltype(helper.getIndexWriter()->snapshots.readers.getReader()) reader;

  void SetUp() override {
    LuxirTest::SetUp();
    std::vector<Doc> docs;
    for (int32_t i = 0; i < kDocs; i++) {
      std::string body = "pad";
      for (int32_t p = 0; p < i % 7; p++) body += " pad";
      if (i % 2 == 0) for (int32_t r = 0; r <= i % 3; r++) body += " alpha";
      if (i % 3 == 0) body += " beta";
      if (i % 5 == 0) body += " gamma";
      docs.push_back(flatdoc("id", std::to_string(i), "body_w", body));
      if (i % 4 != 3) {
        clause.docs.push_back(i);
        clause.scores.push_back(0.5f + (float)((i * 7) % 13) * 0.4f);
        clause.maxScore = std::max(clause.maxScore, clause.scores.back());
      }
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(1u, reader->segments().size());
  }

  // Top k over the single segment, through the pull scorer or the bulk
  // scorer, with competitive pruning on (floors flow) or off (exhaustive).
  Hits run(Query& query, int32_t k, bool bulk, bool pruning) {
    clause.floors.clear();
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context,
        Query::NEED_SCORES | (pruning ? Query::ALLOW_PRUNING : 0));
    auto& segment = reader->segments()[0];
    DocSet* live = segment.liveDocs() ? &segment.liveDocs()->docset() : nullptr;
    TopDocsCollector collector(k);
    if (bulk) {
      auto* supplier = weight->scorerSupplier(pool, segment);
      auto* scorer = supplier ? supplier->bulkScorer(pool) : nullptr;
      bulkMissing = scorer == nullptr;
      if (scorer == nullptr) return {};
      collectTopKWindowed(segment.ord, scorer, live, collector, nullptr,
                          segment.maxDoc(), pruning);
    } else {
      auto* scorer = weight->createScorer(pool, segment);
      EXPECT_NE(scorer, nullptr);
      if (scorer == nullptr) return {};
      collectTopK(segment.ord, scorer, live, nullptr, collector, pruning);
    }
    auto sorted = collector.sort();
    return Hits(sorted.begin(), sorted.end());
  }

  // Pruned run equals the exhaustive run where the clause declares nothing.
  // Returns the largest floor forwarded.
  float expectSame(Query& query, int32_t k, bool bulk,
                   bool requireBulk = true) {
    clause.prunes = false;
    Hits expected = run(query, k, bulk, false);
    clause.prunes = true;
    Hits actual = run(query, k, bulk, true);
    if (bulk && bulkMissing && !requireBulk) return 0.0f;
    EXPECT_FALSE(bulkMissing);
    float maxFloor = 0.0f;
    for (float floor : clause.floors) maxFloor = std::max(maxFloor, floor);
    EXPECT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < std::min(expected.size(), actual.size()); i++) {
      EXPECT_EQ(expected[i].doc, actual[i].doc) << "rank " << i;
      EXPECT_FLOAT_EQ(expected[i].score, actual[i].score) << "rank " << i;
    }
    return maxFloor;
  }
};

TEST_F(ClauseFloorTest, mandatoryPlusFloorClause) {
  TermQuery alpha("body_w", "alpha"), gamma("body_w", "gamma");
  FloorQuery floor(clause);
  Query* must[] = {&alpha};
  Query* opt[] = {&floor};
  Query* filter[] = {&gamma};
  BooleanQuery plain(must, opt, {}, {});
  BooleanQuery filtered(must, opt, {}, filter);
  for (bool bulk : {false, true}) {
    EXPECT_GT(expectSame(plain, 5, bulk), 0.0f) << bulk;
    EXPECT_GT(expectSame(plain, 40, bulk), 0.0f) << bulk;
    EXPECT_EQ(expectSame(plain, 50000, bulk), 0.0f) << bulk;
    EXPECT_GT(expectSame(filtered, 3, bulk), 0.0f) << bulk;
  }
}

TEST_F(ClauseFloorTest, optionalClauses) {
  TermQuery alpha("body_w", "alpha"), beta("body_w", "beta");
  FloorQuery floor(clause);
  Query* two[] = {&floor, &alpha};
  Query* three[] = {&floor, &alpha, &beta};
  BooleanQuery pair({}, two, {}, {});
  BooleanQuery triple({}, three, {}, {});
  for (bool bulk : {false, true}) {
    EXPECT_GT(expectSame(pair, 5, bulk), 0.0f) << bulk;
    EXPECT_GT(expectSame(pair, 100, bulk), 0.0f) << bulk;
    EXPECT_EQ(expectSame(pair, 50000, bulk), 0.0f) << bulk;
    EXPECT_GT(expectSame(triple, 5, bulk), 0.0f) << bulk;
    EXPECT_GT(expectSame(triple, 50, bulk), 0.0f) << bulk;
  }
}

TEST_F(ClauseFloorTest, deletedDocs) {
  ASSERT_TRUE(helper.deleteByIds({"0", "3", "6", "12", "24"},
                                 UpdateMessage::COMMIT).success);
  reader = helper.getIndexWriter()->snapshots.readers.getReader();
  TermQuery alpha("body_w", "alpha");
  FloorQuery floor(clause);
  Query* must[] = {&alpha};
  Query* opt[] = {&floor};
  Query* both[] = {&floor, &alpha};
  BooleanQuery mandOpt(must, opt, {}, {});
  BooleanQuery disj({}, both, {}, {});
  for (bool bulk : {false, true}) {
    EXPECT_GT(expectSame(mandOpt, 5, bulk), 0.0f) << bulk;
    EXPECT_GT(expectSame(disj, 5, bulk), 0.0f) << bulk;
  }
}

TEST_F(ClauseFloorTest, neverForwardedUnderDisMax) {
  TermQuery alpha("body_w", "alpha");
  FloorQuery floor(clause);
  MemPool pool;
  Query* parts[] = {&floor, &alpha};
  auto* dismax = DisMaxQuery::make(pool, parts, 0.3f);
  ASSERT_EQ(QueryKind::DIS_MAX, dismax->getKind());
  for (bool bulk : {false, true}) {
    EXPECT_EQ(expectSame(*dismax, 5, bulk, false), 0.0f) << bulk;
  }
}

TEST(ClauseFloor, edgeCases) {
  constexpr float inf = std::numeric_limits<float>::infinity();
  constexpr double dinf = std::numeric_limits<double>::infinity();
  constexpr float nan = std::numeric_limits<float>::quiet_NaN();
  auto none = [](float floor) { return floor == -inf; };
  EXPECT_TRUE(none(clauseFloor(inf, 1.0, 2)));
  EXPECT_TRUE(none(clauseFloor(-inf, 1.0, 2)));
  EXPECT_TRUE(none(clauseFloor(nan, 1.0, 2)));
  EXPECT_TRUE(none(clauseFloor(10.0f, dinf, 2)));
  EXPECT_TRUE(none(clauseFloor(10.0f, std::nan(""), 2)));
  // The others alone can reach theta, so no clause floor is proven.
  EXPECT_TRUE(none(clauseFloor(1.0f, 2.0, 2)));
  EXPECT_TRUE(none(clauseFloor(2.0f, 2.0, 2)));
  EXPECT_TRUE(none(clauseFloor(2.0f, 1.9999999, 2)));
  EXPECT_GT(clauseFloor(2.0f, 1.0, 2), 0.0f);
  // Negative others (a bound below zero) still give a floor under theta.
  EXPECT_GT(clauseFloor(1.0f, -0.5, 2), 0.0f);
}

TEST(ClauseFloor, roundingSlackKeepsEveryCompetitiveScore) {
  for (size_t clauses : {2u, 3u, 8u}) {
    for (float theta : {0.01f, 1.0f, 3.7f, 123.456f, 1e6f}) {
      for (double others : {0.0, 0.3, 1.25, 3.0, 100.0}) {
        float floor = clauseFloor(theta, others, clauses);
        if (floor == -std::numeric_limits<float>::infinity()) continue;
        ASSERT_GT(floor, 0.0f);
        // Never above the exact value.
        EXPECT_LT((double)floor, (double)theta - others);
        // A score just under the floor cannot make theta in float summation.
        float below = std::nextafter(floor, 0.0f);
        EXPECT_LT(below + (float)others, theta)
            << theta << " " << others << " " << clauses;
        // A larger theta never lowers the floor.
        EXPECT_LE(floor, clauseFloor(std::nextafter(theta, 1e30f), others, clauses));
      }
    }
  }
}

}  // namespace
