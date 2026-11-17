// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/AllQuery.h"
#include "luxir/query/ImpactsIndex.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/OwnershipRelation.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"

namespace luxir::test {
namespace {

std::vector<std::pair<std::string, float>> rankedHits(const LocalReq& req) {
  std::vector<std::pair<std::string, float>> hits;
  for (auto* response : req.responses) {
    auto* result = response->proto.ops.find("q");
    if (!result || !(**result).docList()) continue;
    const auto& columns = (**result).docList()->columns;
    auto* ids = columns.find("id");
    if (!ids) continue;
    auto names = std::get<api::ColStr>(ids->kind).v;
    auto scores = std::get<api::ColFloat>(columns.find("_score_")->kind).v;
    for (size_t i = 0; i < names.size(); i++) hits.emplace_back(names[i], scores[i]);
  }
  return hits;
}

class JoinWindowTest : public LuxirTest {
protected:
  CollectionHelper helper;
};

TEST_F(JoinWindowTest, exposureSelectsPullOrWindowedFilteredRanking) {
  constexpr int32_t N = DocsEnumMeta::L1_DOCS + 512;
  SearchOverridesGuard impacts(ImpactsIndex::forceEagerForTests);
  ImpactsIndex::forceEagerForTests = true;
  helper.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes = 0});
  CollectionHelper::UpdateBuilder input;
  for (int32_t i = 0; i < N; i++) {
    std::string json = "{\"id\":\"" + std::to_string(i) + "\",\"body_w\":\"alpha"
        + (i % 2 ? " beta beta" : " gamma") + (i % 3 ? " alpha" : " gamma beta")
        + (i % 127 == 0 ? " rare" : "")
        + "\",\"items_nested\":{\"color_s\":\"" + (i % 2 ? "red" : "blue") + "\"}}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto root = helper.getIndexWriter()->snapshots.readers.getReader();
  auto many = root->view("items_nested");
  MemPool pool;
  Query::Context context(pool, *many);
  OwnershipRelation relation(*many, *root, context.planningContext().memoryTracker);
  TermQuery term("items_nested.color_s", "red");
  for (bool maximum : {false, true}) {
    auto* weight = term.createWeight(context, maximum ? Query::NEED_SCORES : 0);
    auto* source = weight->scorerSupplier(pool, many->segments()[0]);
    ASSERT_NE(nullptr, source);
    auto* supplier = relation.reduce(pool, 0, *source, nullptr, RowReduction{maximum ? RowReduction::MAXIMUM : RowReduction::FIXED}, 1.0f);
    for (int64_t candidates : {1, N}) {
      for (auto consumption : {Query::MembershipConsumption::EITHER, Query::MembershipConsumption::WINDOWS,
                               Query::MembershipConsumption::CANDIDATES}) {
        auto planContext = Query::PlanContext::fromLeadCost(candidates, Query::ExecutionUse::MATCH_WINDOWS);
        planContext.demand.span = N;
        planContext.membershipConsumption = consumption;
        auto shape = supplier->describeScorer(planContext);
        auto* plan = supplier->resolve(pool, planContext);
        EXPECT_EQ(Query::MatchState::NONEMPTY, plan->shape().matchState);
        EXPECT_EQ(Query::DirectScorerKind::OTHER, plan->shape().directKind);
        bool probing = !maximum && (consumption == Query::MembershipConsumption::CANDIDATES || candidates == 1);
        EXPECT_EQ(probing ? Query::ReportedTwoPhase::YES : Query::ReportedTwoPhase::NO,
            plan->shape().reportedTwoPhase);
        EXPECT_EQ(!maximum && ((probing && consumption != Query::MembershipConsumption::WINDOWS) || candidates == N) ? Query::ClauseShape::DIRECT : Query::ClauseShape::NONE,
            plan->shape().windowFillClause);
        EXPECT_EQ(shape.windowFillClause, plan->shape().windowFillClause);
        EXPECT_EQ(consumption == Query::MembershipConsumption::EITHER
            ? (probing ? Query::MembershipConsumption::CANDIDATES : Query::MembershipConsumption::WINDOWS)
            : consumption, plan->shape().membershipConsumption);
        if (!maximum && candidates == 1 && consumption == Query::MembershipConsumption::WINDOWS) {
          // Explicit window consumption remains valid when it is not advertised.
          auto* scorer = plan->build(pool);
          std::array<uint64_t, 1> bits{};
          scorer->fillWindowBits(bits, 0, 64);
          EXPECT_EQ(0xaaaaaaaaaaaaaaaaULL, bits[0]);
        }
      }
    }
    EXPECT_EQ(Query::ClauseShape::NONE, supplier->describeScorer({}).windowFillClause);
    auto expected = Query::PlanContext::fromLeadCost(N, Query::ExecutionUse::MATCH_WINDOWS);
    expected.demand.expectedCandidates = 1;
    EXPECT_EQ(Query::ClauseShape::NONE, supplier->describeScorer(expected).windowFillClause);
    // The same candidate count can fill profitably over a short exposure.
    expected.expectedSpan = 1;
    EXPECT_EQ(maximum ? Query::ClauseShape::NONE : Query::ClauseShape::DIRECT,
        supplier->describeScorer(expected).windowFillClause);
  }
  auto run = [&](int32_t position, bool selective = false) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().exprQuery(selective ? "body_w:(+rare alpha beta)"
        : "body_w:(alpha OR beta OR gamma)").getScores().fields({"id"});
    op.limit(position ? 7 : -1);
    if (position) {
      auto join = qb::toParent(req->mr, "items_nested", qb::match(req->mr, "color_s", "red"), "none");
      if (position == 1) op.filter(join);
      else op.rawQuery() = qb::boolean(req->mr, {}, {qb::match(req->mr, "body_w", "alpha"),
          qb::match(req->mr, "body_w", "beta"), qb::match(req->mr, "body_w", "gamma")}, {join});
    }
    SkipStatsScope stats;
    req->execute(1); EXPECT_OK(req);
    if (position && selective) {
      EXPECT_EQ(0, SkipStats::countBulkFillCalls);
      EXPECT_EQ(0, SkipStats::maxScoreOuterWindows);
    } else if (position) {
      EXPECT_GT(SkipStats::maxScoreOuterWindows, 0);
      EXPECT_GT(SkipStats::countBulkFillCalls, 0);
      if (position == 2) { EXPECT_GT(SkipStats::bulkExclusionFills, 0); }
    }
    return req;
  };
  for (bool selective : {false, true}) {
    auto all = run(0, selective);
    auto hits = rankedHits(*all);
    ASSERT_EQ(selective ? (size_t)(1 + (N - 1) / 127) : (size_t)N, hits.size());
    for (int32_t position : {1, 2}) {
      if (selective && position == 2) continue;
      auto selected = run(position, selective);
      auto selectedHits = rankedHits(*selected);
      size_t next = 0;
      for (const auto& [id, score] : hits) {
        if (next == 7) break;
        if (std::stoi(id) % 2 != (position == 1 ? 1 : 0)) continue;
        ASSERT_LT(next, selectedHits.size());
        EXPECT_EQ(id, selectedHits[next].first);
        EXPECT_FLOAT_EQ(score, selectedHits[next++].second);
      }
      EXPECT_EQ(7u, next);
    }
  }
  auto count = localReq(helper.getSearchEngine());
  count->topDocs().exprQuery("body_w:(beta AND gamma)").countLimit().limit(0).filter(qb::toParent(count->mr,
      "items_nested", qb::match(count->mr, "color_s", "red"), "none"));
  SkipStatsScope stats;
  count->execute(1); ASSERT_OK(count); EXPECT_EQ(N / 6, count->getMatchCount());
  EXPECT_GT(SkipStats::countBulkFillCalls, 0);
  EXPECT_GT(SkipStats::conjDenseCountWindows, 0);
}

TEST_F(JoinWindowTest, boundedFillsCrossFamiliesEmptyWindowsAndDeletedOwners) {
  constexpr int32_t W = DocsEnumMeta::L1_DOCS;
  constexpr int32_t N = 2 * W + 19;
  const std::array<int32_t, 4> owners{0, W - 1, 2 * W + 1, N - 1};
  CollectionHelper::UpdateBuilder input;
  std::vector<int32_t> redRows;
  int32_t row = 0;
  for (int32_t owner = 0; owner < N; owner++) {
    int32_t count = owner == 0 ? W + 5 : owner == W - 1 ? W + 7
        : owner == 2 * W + 1 ? 3 : owner == N - 1 ? 2 : 0;
    std::string json = "{\"id\":\"" + std::to_string(owner) + "\",\"items_nested\":[";
    for (int32_t i = 0; i < count; i++, row++) {
      bool red = i == count - 1 || (owner == W - 1 && i == 0);
      if (red) redRows.push_back(row);
      if (i) json += ',';
      json += "{\"color_s\":\"" + std::string(red ? "red" : "blue") + "\",\"price_i\":" + (red ? "1" : "0")
          + ",\"text_w\":\"" + (red ? "alpha beta" : "alpha x beta")
          + "\",\"parts_nested\":{\"color_s\":\"" + (red ? "red" : "blue") + "\"}}";
    }
    json += "]}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  for (auto owner : {W - 1, N - 1}) ASSERT_TRUE(helper.deleteById(std::to_string(owner), UpdateMessage::COMMIT).success);
  auto root = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, root->segments().size());
  const std::array<std::pair<int32_t, int32_t>, 6> windows{{
      {0, 1}, {1, W - 1}, {W - 1, W}, {W, 2 * W}, {2 * W, 2 * W + 1}, {2 * W + 1, N}}};
  for (auto path : {"items_nested", "items_nested.parts_nested"}) {
    auto many = root->view(path);
    MemPool pool;
    Query::Context context(pool, *many);
    OwnershipRelation relation(*many, *root, context.planningContext().memoryTracker);
    std::string field = std::string(path) + ".color_s", price = std::string(path) + ".price_i";
    std::string text = std::string(path) + ".text_w";
    TermQuery term(field, "red");
    AllQuery all;
    NumericPredicateQuery numeric(price, 1, 1);
    std::string_view terms[]{"alpha", "beta"}; int32_t positions[]{0, 1};
    PhraseQuery phrase(text, terms, positions);
    const std::array<Query*, 5> queries{&term, &all, nullptr, &numeric, &phrase};
    ArrDocSet cached{std::vector<int32_t>(redRows)};
    for (int kind = 0; kind < (many->path() == "items_nested" ? 5 : 3); kind++) {
      for (bool live : {false, true}) {
        SCOPED_TRACE(::testing::Message() << path << '/' << kind << '/' << live);
        auto* source = kind == 2 ? pool.make<QueryPrep::DocSetSupplier>(&cached, many->segments()[0])
            : queries[(size_t)kind]->createWeight(context, 0)->scorerSupplier(pool, many->segments()[0]);
        ASSERT_NE(nullptr, source);
        auto* supplier = relation.reduce(pool, 0, *source, live ? root->segments()[0].liveDocSet() : nullptr, RowReduction{RowReduction::FIXED}, 1.0f);
        auto planContext = Query::PlanContext::fromLeadCost(N, Query::ExecutionUse::MATCH_WINDOWS);
        auto* scorer = supplier->resolve(pool, planContext)->build(pool);
        ASSERT_NE(nullptr, scorer);
        for (auto [lo, hi] : windows) {
          std::array<uint64_t, W / 64> bits{};
          bits.back() = 1ULL << 63; // OR filling must retain unrelated caller bits.
          scorer->fillWindowBits(bits, lo, hi);
          for (int32_t i = 0; i < hi - lo; i++) {
            bool expected = std::ranges::find(owners, lo + i) != owners.end()
                && (!live || (lo + i != W - 1 && lo + i != N - 1));
            expected |= i == W - 1;
            EXPECT_EQ(expected, (bits[(size_t)i >> 6] & (1ULL << (i & 63))) != 0) << lo + i;
          }
          EXPECT_TRUE(bits.back() & (1ULL << 63));
          for (int32_t i = hi - lo; i < W - 1; i++) EXPECT_EQ(0ULL, bits[(size_t)i >> 6] & (1ULL << (i & 63)));
        }
        // Bounded probes and fills share the same child membership protocol.
        auto* probe = supplier->resolve(pool, planContext)->build(pool);
        EXPECT_EQ(0, probe->advanceWindow(0, 1));
        std::array<uint64_t, W / 64> bits{};
        probe->fillWindowBits(bits, W - 1, W);
        EXPECT_EQ(!live, (bits[0] & 1) != 0);
        EXPECT_GE(probe->advanceWindow(W, 2 * W), 2 * W);
        EXPECT_EQ(2 * W + 1, probe->advanceWindow(2 * W + 1, N));
      }
    }
  }
}

TEST_F(JoinWindowTest, composedFillsStayBoundedAcrossLargeAndEmptyIntermediateFamilies) {
  constexpr int32_t W = DocsEnumMeta::L1_DOCS;
  constexpr int32_t N = W + 11;
  std::vector<bool> itemMatches(N), partMatches(N), intermediateMatches;
  int32_t item = 0;
  CollectionHelper::UpdateBuilder input;
  for (int32_t owner = 0; owner < N; owner++) {
    int32_t count = owner == 0 ? 2 * W + 17 : owner == 7 ? 3 : owner == W - 3 || owner == N - 1 ? 2 : 0;
    std::string json = "{\"id\":\"" + std::to_string(owner) + "\",\"items_nested\":[";
    for (int32_t i = 0; i < count; i++, item++) {
      bool red = i % 2 == 0;
      itemMatches[(size_t)owner] = itemMatches[(size_t)owner] || red;
      bool empty = owner == 7 || item < 4 || (item >= W - 3 && item < W + 3)
          || (owner == 0 && i >= count - 4);
      int32_t parts = empty ? 0 : 1 + item % 3;
      intermediateMatches.push_back(parts != 0);
      partMatches[(size_t)owner] = partMatches[(size_t)owner] || parts != 0;
      if (i) json += ',';
      json += "{\"color_s\":\"" + std::string(red ? "red" : "blue") + "\",\"text_w\":\""
          + (red ? "hit here" : "hit x here") + "\",\"parts_nested\":[";
      for (int32_t p = 0; p < parts; p++) {
        if (p) json += ',';
        json += "{\"color_s\":\"" + std::string(p == parts - 1 ? "red" : "blue") + "\",\"text_w\":\""
            + (p == parts - 1 ? "hit here" : "hit x here") + "\"}";
      }
      json += "]}";
    }
    json += "]}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto root = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, root->segments().size());
  const char* paths[]{"items_nested", "items_nested.parts_nested"};
  for (int pair = 0; pair < 3; pair++) {
    auto one = pair == 2 ? root->view(paths[0]) : root;
    auto many = root->view(paths[pair == 0 ? 0 : 1]);
    const auto& expected = pair == 0 ? itemMatches : pair == 1 ? partMatches : intermediateMatches;
    ASSERT_EQ((int64_t)expected.size(), one->maxDoc());
    MemPool pool;
    Query::Context context(pool, *many);
    OwnershipRelation relation(*many, *one, context.planningContext().memoryTracker);
    std::string field = std::string(many->path()) + ".color_s", text = std::string(many->path()) + ".text_w";
    TermQuery term(field, "red");
    std::string_view terms[]{"hit", "here"}; int32_t positions[]{0, 1};
    PhraseQuery phrase(text, terms, positions);
    AllQuery all;
    for (auto* query : std::array<Query*, 3>{&term, &phrase, &all}) {
      auto* source = query->createWeight(context, 0)->scorerSupplier(pool, many->segments()[0]);
      ASSERT_NE(nullptr, source);
      auto* supplier = relation.reduce(pool, 0, *source, nullptr, RowReduction{RowReduction::FIXED}, 1.0f);
      for (bool whole : {false, true}) {
        SCOPED_TRACE(::testing::Message() << pair << '/' << (int)query->getKind() << '/' << whole);
        auto* scorer = buildScorerForTests(pool, *supplier, one->maxDoc());
        for (int32_t first = 0; first < one->maxDoc();) {
          int32_t end = whole ? (int32_t)one->maxDoc() : std::min((int32_t)one->maxDoc(), first + W);
          std::vector<uint64_t> bits((size_t)(end - first + 63) / 64);
          scorer->fillWindowBits(bits, first, end);
          for (int32_t row = first; row < end; row++) {
            bool matched = (bits[(size_t)(row - first) / 64] & (1ULL << ((row - first) & 63))) != 0;
            EXPECT_EQ(expected[(size_t)row], matched) << row;
          }
          first = end;
        }
      }
    }
  }
}

} // namespace
} // namespace luxir::test
