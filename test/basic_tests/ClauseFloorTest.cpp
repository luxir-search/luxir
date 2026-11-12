// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "luxir/query/BooleanWeight.h"
#include "luxir/query/DisMaxQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/Collector.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"
#include "test/TopKAssert.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Hits = std::vector<TopDocsCollector::DocScore>;
using SumScorer = BooleanQuery::MaxScoreDisjunctionScorer;
using DisMaxScorer = BooleanQuery::MaxScoreDisjunctionScorerT<true>;

int64_t visited(Query::Scorer* scorer) {
  std::span<Query::Scorer*> children;
  int64_t count = 0;
  if (auto* dismax = dynamic_cast<DisMaxScorer*>(scorer)) {
    count = dismax->visited();
    children = dismax->clauseScorersForTests();
  } else if (auto* sum = dynamic_cast<SumScorer*>(scorer)) {
    count = sum->visited();
    children = sum->clauseScorersForTests();
  } else if (auto* mand = dynamic_cast<BooleanQuery::MandOptScorer*>(scorer)) {
    return visited(mand->requiredScorerForTests()) + visited(mand->optionalScorerForTests());
  } else if (auto* conjunction = dynamic_cast<BooleanQuery::ConjunctionScorer*>(scorer)) {
    children = conjunction->scoringScorersForTests();
  } else if (auto* excluded = dynamic_cast<BooleanQuery::MandNotScorer*>(scorer)) {
    return visited(excluded->requiredScorerForTests());
  } else if (auto* heap = dynamic_cast<BooleanQuery::DisjunctionScorerT<true>*>(scorer)) {
    children = heap->clauseScorersForTests();
  } else if (auto* heap = dynamic_cast<BooleanQuery::DisjunctionScorerT<false>*>(scorer)) {
    children = heap->clauseScorersForTests();
  }
  for (auto* child : children) count += visited(child);
  return count;
}

class ClauseFloorTest : public LuxirTest {
public:
  CollectionHelper helper;
  decltype(helper.getIndexWriter()->snapshots.readers.getReader()) reader;

  void SetUp() override {
    LuxirTest::SetUp();
    std::mt19937 rng(235244);
    auto text = [&](int count) {
      std::string out;
      for (int i = 0; i < count; i++) {
        out += " t" + std::to_string((int)std::pow(16.0, (double)(rng() % 10000) / 10000.0) - 1);
      }
      return out;
    };
    std::vector<Doc> docs;
    for (int i = 0; i < 5 * DocsEnumMeta::L1_DOCS; i++) {
      std::string body = text(20 + (int)(rng() % 150));
      if (i % 17 != 0) body += " req";
      if (i % 3 != 0) body += " keep";
      if (i % 19 == 0) body += " omit";
      docs.push_back(flatdoc("id", std::to_string(i), "title_w", text(1 + (int)(rng() % 8)),
                             "body_w", body));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(1u, reader->segments().size());
  }

  struct Result {
    Hits hits;
    int64_t visits = 0;
  };

  Result run(Query& query, bool bulk, bool pruning, bool forward = true) {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context,
        Query::NEED_SCORES | (pruning ? Query::ALLOW_PRUNING : 0));
    TopDocsCollector collector(5);
    Result result;
    auto& segment = reader->segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    EXPECT_NE(nullptr, supplier);
    if (supplier == nullptr) return result;
    DocSet* live = segment.liveDocs() ? &segment.liveDocs()->docset() : nullptr;
    if (bulk) {
      Query::ScorerSupplier::BulkScorerContext buildContext;
      auto plan = supplier->planBulk(Query::ExecutionUse::MATCH_WINDOWS, buildContext);
      auto* scorer = plan.available == Query::ScorerSupplier::BulkAnswer::YES
          ? supplier->buildBulk(pool, plan) : nullptr;
      EXPECT_NE(nullptr, scorer);
      if (scorer == nullptr) return result;
      collectTopKWindowed(segment.ord, scorer, live, collector, nullptr, segment.maxDoc(), pruning);
      if (auto* mand = dynamic_cast<BooleanQuery::MandOptBulkScorer*>(scorer)) {
        for (auto* child : mand->optionalScorersForTests()) result.visits += visited(child);
      } else if (auto* sum = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(scorer)) {
        for (auto* child : sum->clauseScorersForTests()) result.visits += visited(child);
      } else {
        ADD_FAILURE() << "Expected MandOptBulkScorer or MaxScoreBulkScorer";
      }
    } else {
      auto buildContext = supplier->makePlanContext(
          Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max()));
      buildContext.disableClauseFloorForwardingForTests = !forward;
      auto* scorer = supplier->resolve(pool, buildContext)->build(pool);
      collectTopK(segment.ord, scorer, live, nullptr, collector, pruning);
      result.visits = visited(scorer);
    }
    auto hits = collector.sort();
    result.hits.assign(hits.begin(), hits.end());
    return result;
  }

  bool floorPays(Query& query, int64_t candidates) {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);
    auto buildContext = supplier->makePlanContext(Query::Demand::fromLeadCost(candidates));
    return supplier->resolve(pool, buildContext)->shape().competitiveFloorPays;
  }

  void exercise() {
    MemPool pool;
    auto field = [&](const char* name) -> Query* {
      auto clauses = pool.make_span<Query*>(4);
      const char* terms[] = {"t0", "t1", "t2", "t3"};
      for (int i = 0; i < 4; i++) clauses[(size_t)i] = pool.make<TermQuery>(name, terms[i]);
      return pool.make<BooleanQuery>(std::span<Query*>{}, clauses,
                                     std::span<Query*>{}, std::span<Query*>{});
    };
    Query* fields[] = {field("title_w"), field("body_w")};
    Query* dismax = DisMaxQuery::make(pool, fields, 0.0f);
    TermQuery req("body_w", "req"), keep("body_w", "keep"), omit("body_w", "omit");
    Query* must[] = {&req};
    Query* optional[] = {dismax};
    Query* filter[] = {&keep};
    Query* prohibited[] = {&omit};
    Query* pair[] = {dismax, &req};
    BooleanQuery required(must, optional, {}, {}), sum({}, pair, {}, {});
    BooleanQuery filtered({}, optional, {}, filter, 1), excluded({}, optional, prohibited, {});
    Query* sumGroup[] = {field("body_w")};
    BooleanQuery grouped(sumGroup, {}, {}, {});
    Query* groupedOptional[] = {&grouped};
    BooleanQuery requiredSum(must, groupedOptional, {}, {});
    Query* filteredOptional[] = {&filtered};
    Query* excludedOptional[] = {&excluded};
    BooleanQuery wrapped(must, filteredOptional, {}, {}), withExclusion(must, excludedOptional, {}, {});
    BooleanQuery requiredFiltered(optional, {}, {}, filter);
    constexpr int64_t unbounded = std::numeric_limits<int64_t>::max();
    EXPECT_TRUE(floorPays(*dismax, unbounded));
    EXPECT_TRUE(floorPays(grouped, unbounded));
    EXPECT_FALSE(floorPays(grouped, 1));
    EXPECT_FALSE(floorPays(*dismax, 1));
    EXPECT_FALSE(floorPays(req, unbounded));
    EXPECT_TRUE(floorPays(filtered, 1));
    EXPECT_TRUE(floorPays(excluded, unbounded));
    EXPECT_FALSE(floorPays(requiredFiltered, unbounded));
    bool floorArrived = false;
    for (bool bulk : {false, true}) {
      for (Query* query : std::array<Query*, 5>{&required, &sum, &requiredSum, &wrapped, &withExclusion}) {
        SCOPED_TRACE(::testing::Message() << "bulk " << bulk << " query " << query);
        Result exhaustive = run(*query, bulk, false);
        Result on = run(*query, bulk, true);
        assertTopKEquivalent(exhaustive.hits, on.hits);
        // Only pull plans carry the switch that withholds floors.
        if (bulk) continue;
        Result off = run(*query, bulk, true, false);
        assertTopKEquivalent(exhaustive.hits, off.hits);
        floorArrived |= on.visits < off.visits;
      }
    }
    EXPECT_TRUE(floorArrived);
  }
};

TEST_F(ClauseFloorTest, realQueries) { exercise(); }

TEST_F(ClauseFloorTest, deletedDocs) {
  ASSERT_TRUE(helper.deleteByIds({"0", "3", "6", "12", "24"},
                                 UpdateMessage::COMMIT).success);
  reader = helper.getIndexWriter()->snapshots.readers.getReader();
  exercise();
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
  // Negative other contributions are conservatively bounded by zero.
  EXPECT_EQ(clauseFloor(1.0f, -0.5, 2), clauseFloor(1.0f, 0.0, 2));
  EXPECT_TRUE(none(clauseFloor(0.0f, -1.0, 2)));
  EXPECT_TRUE(none(clauseFloor(-1.0f, -1.01, 1001)));
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
