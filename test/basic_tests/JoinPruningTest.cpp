// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "luxir/query/ImpactsIndex.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/ops/TopDocsReq.h"

namespace luxir::test {
namespace {

class JoinPruningTest : public LuxirTest {
protected:
  struct Run {
    std::vector<Doc> docs;
    std::vector<float> scores;
    int64_t count;
    int64_t decoded;
    int64_t skips;
    bool gte;
    bool estimated;
  };

  void index(CollectionHelper& helper, int32_t segments) {
    for (int32_t segment = 0; segment < segments; segment++) {
      CollectionHelper::UpdateBuilder input;
      for (int32_t root = 0; root < 2048; root++) {
        std::string json = "{\"id\":\"" + std::to_string(segment * 2048 + root)
            + "\",\"body_w\":\"root " + (root % 3 ? std::string("other") : std::string("extra"))
            + "\",\"chunks_nested\":[";
        int32_t children = root % 31 == 0 ? 17 : root % 4 + 1;
        for (int32_t child = 0; child < children; child++) {
          std::string text;
          // Strong regions separated by weak blocks exercise skips and resumes.
          bool strong = root < 128 || (root >= 1024 && root < 1040);
          int32_t repeats = strong ? 12 + (root + child) % 13 : 1;
          for (int32_t i = 0; i < repeats; i++) text += "alpha beta ";
          if (root % 11 == 0) text += "rare ";
          if (root % 127 == 0) text += "scarce ";
          for (int32_t i = 0; i < (strong ? root % 9 : 100 + root % 23); i++) text += "pad ";
          if (child) json += ',';
          json += "{\"text_w\":\"" + text + "\"}";
        }
        json += "]}";
        input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
      }
      input.commit();
      ASSERT_TRUE(helper.submit(input).success);
    }
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ((size_t)segments, reader->segments().size());
  }

  Run run(CollectionHelper& helper, int32_t family, int32_t outer, bool reversed,
          float boost, int32_t k, bool count, int64_t countLimit = -1,
          std::string_view mode = "max", int32_t minimum = 1, int32_t maximum = INT32_MAX) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(helper.collection().getId().name);
    auto& op = req->topDocs().fields({"id"}).getScores().limit(k);
    if (count) op.countLimit(countLimit);
    auto& mr = req->mr;
    auto a = qb::match(mr, "text_w", "alpha");
    api::Query inner = family == 0 ? a : family == 1 ? qb::boolean(mr, {}, {a, qb::match(mr, "text_w", "rare"), qb::match(mr, "text_w", "scarce")})
        : family == 2 ? qb::boolean(mr, {a, qb::match(mr, "text_w", "rare")}) : qb::phraseText(mr, "text_w", "alpha beta");
    auto joined = qb::toParent(mr, "chunks_nested", inner, mode);
    auto& spec = std::get<api::ToParentQuery>(joined.kind);
    spec.min_children = minimum; spec.max_children = maximum;
    auto join = qb::boost(mr, joined, boost);
    auto root = qb::match(mr, "body_w", "root");
    auto first = reversed ? root : join;
    auto second = reversed ? join : root;
    op.rawQuery() = outer == 0 ? join : outer == 1 ? qb::boolean(mr, {first, second})
        : qb::boolean(mr, {}, {first, second});
    SkipStatsScope stats;
    req->execute(1);
    EXPECT_OK(req);
    Run out{req->getDocs(), {}, count ? req->getMatchCount() : -1,
        SkipStats::docBlocksDecoded,
        SkipStats::conjRangeSkips + SkipStats::wandAdvancePrunes + SkipStats::wandCandidatePrunes
            + SkipStats::maxScoreDeadOuterJumps + SkipStats::maxScoreBufferCompactions
            + SkipStats::phraseImpactBlocksSkipped,
        req->docList() && req->docList()->found_gte, req->docList() && req->docList()->found_estimated};
    if (auto* docs = req->docList(); docs && docs->row_count) {
      auto scores = std::get<api::ColFloat>(docs->columns.find("_score_")->kind).v;
      out.scores.assign(scores.begin(), scores.end());
    }
    return out;
  }

  void compare(const Run& exhaustive, const Run& pruned) {
    ASSERT_EQ(exhaustive.docs.size(), pruned.docs.size());
    ASSERT_EQ(exhaustive.scores.size(), pruned.scores.size());
    for (size_t i = 0; i < exhaustive.docs.size(); i++) {
      EXPECT_TRUE(docEquals(exhaustive.docs[i], pruned.docs[i])) << i;
      EXPECT_FLOAT_EQ(exhaustive.scores[i], pruned.scores[i]) << i;
    }
  }
};

TEST_F(JoinPruningTest, everyReductionAndBoundMatchesAnUnprunedTopK) {
  SearchOverridesGuard impacts(ImpactsIndex::forceEagerForTests);
  SearchOverridesGuard composition(disableTopKCountComposition);
  disableTopKCountComposition = true; ImpactsIndex::forceEagerForTests = true;
  CollectionHelper helper("all_join_reductions"); index(helper, 2);
  ASSERT_TRUE(helper.deleteByIds({"0","1","2050"}, UpdateMessage::COMMIT).success);
  for (auto mode : {"max","min","sum","avg","none"}) for (bool bounded : {false,true}) {
    for (int family : {0,1,2,3}) for (int outer : {0,1,2}) {
      SCOPED_TRACE(::testing::Message() << mode << '/' << bounded << '/' << family << '/' << outer);
      auto exhaustive = run(helper,family,outer,false,0.7f,5,true,-1,mode,bounded ? 2 : 1,bounded ? 3 : INT32_MAX);
      auto pruned = run(helper,family,outer,false,0.7f,5,false,-1,mode,bounded ? 2 : 1,bounded ? 3 : INT32_MAX);
      compare(exhaustive,pruned);
    }
  }
}

TEST_F(JoinPruningTest, maximumTopKMatchesExhaustiveWithBoostsDeletesAndBooleanClauses) {
  SearchOverridesGuard impacts(ImpactsIndex::forceEagerForTests);
  for (int32_t segments : {1, 3}) {
    ImpactsIndex::forceEagerForTests = segments == 1;
    CollectionHelper helper("join_pruning_" + std::to_string(segments));
    index(helper, segments);
    for (bool deleted : {false, true}) {
      if (deleted) {
        CollectionHelper::UpdateBuilder input;
        for (int32_t id : {0, 1, 7, 1025, 2050, 4100}) input.remove(std::to_string(id));
        input.commit();
        ASSERT_TRUE(helper.submit(input).success);
      }
      for (int32_t family = 0; family < 4; family++) {
        for (float boost : {1.0f, 0.3f, 2.5f, 0.0f}) {
          for (int32_t k : {5, 7000}) {
            SCOPED_TRACE(::testing::Message() << segments << '/' << deleted << '/' << family << '/' << boost << '/' << k);
            auto exhaustive = run(helper, family, 0, false, boost, k, true);
            auto pruned = run(helper, family, 0, false, boost, k, false);
            compare(exhaustive, pruned);
            int64_t matches = family == 2 ? segments * 187 - (deleted ? 1 : 0)
                : segments * 2048 - (deleted ? segments == 1 ? 4 : 6 : 0);
            EXPECT_EQ(matches, exhaustive.count);
            EXPECT_FALSE(exhaustive.gte);
            EXPECT_FALSE(exhaustive.estimated);
            if (k == 5 && boost > 0) {
              if (family < 2) { EXPECT_LT(pruned.decoded, exhaustive.decoded); }
              else { EXPECT_GT(pruned.skips, 0); }
            }
          }
        }
        auto exhaustive = run(helper, family, 0, false, 1.0f, 5, true);
        auto bounded = run(helper, family, 0, false, 1.0f, 5, true, 16);
        compare(exhaustive, bounded);
        EXPECT_GE(bounded.count, 16);
        EXPECT_TRUE(bounded.gte || bounded.estimated || bounded.count == exhaustive.count);
        for (int32_t outer : {1, 2}) {
          for (bool reversed : {false, true}) {
            auto exhaustive = run(helper, family, outer, reversed, 0.3f, 7, true);
            compare(exhaustive, run(helper, family, outer, reversed, 0.3f, 7, false));
          }
        }
      }
    }
  }
}

TEST_F(JoinPruningTest, varyingRootScoresUseTheGenericBoundedCountPass) {
  CollectionHelper helper("join_bounded_split");
  index(helper, 3);
  helper.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes = 0});
  auto run = [&](int64_t count) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("join_bounded_split");
    auto& op = req->topDocs().getScores().limit(10).fields({"id"});
    op.rawQuery() = qb::boolean(req->mr, {qb::match(req->mr, "body_w", "root"),
        qb::toParent(req->mr, "chunks_nested", qb::exists(req->mr, "text_w"))});
    if (count) op.countLimit(count);
    req->execute(1); EXPECT_OK(req);
    return req;
  };
  auto uncounted = run(0);
  SkipStatsScope stats;
  auto bounded = run(10);
  EXPECT_EQ(10, bounded->getMatchCount());
  EXPECT_TRUE(bounded->docList()->found_gte);
  EXPECT_EQ(1, SkipStats::boundedCountSegments);
  auto hits = [&](auto& req) {
    std::vector<std::pair<std::string, float>> result;
    auto names = std::get<api::ColStr>(req->docList()->columns.at("id").kind).v;
    auto scores = std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v;
    for (size_t i = 0; i < names.size(); i++) result.emplace_back(names[i], scores[i]);
    return result;
  };
  EXPECT_EQ(hits(uncounted), hits(bounded));
  auto exact = run(-1);
  EXPECT_EQ(6144, exact->getMatchCount());
  EXPECT_EQ(hits(exact), hits(bounded));
}

TEST_F(JoinPruningTest, uniformChildNegationKeepsItsMembershipProtocolAcrossFamilies) {
  CollectionHelper helper("join_uniform_negation");
  CollectionHelper::UpdateBuilder input;
  std::string first = R"({"id":"a","items_nested":[)";
  for (int i = 0; i < 640; i++) {
    if (i) first += ',';
    first += i % 2 ? R"({"color_s":"blue"})" : R"({"color_s":"red"})";
  }
  first += "]}";
  for (auto json : {first, std::string(R"({"id":"b","items_nested":{"color_s":"blue"}})")}) {
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto req = localReq(helper.getSearchEngine());
  req->collection("join_uniform_negation");
  req->topDocs().exprQuery("items_nested:(NOT color_s:blue)").getScores().fields({"id"}).limit(-1).countLimit();
  req->execute(1); ASSERT_OK(req);
  EXPECT_EQ(1, req->getMatchCount());
  EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("id", "a")));
}

TEST_F(JoinPruningTest, uniformMaximumStopsAtThePageOrBoundedCount) {
  constexpr int32_t N = 50000;
  constexpr int32_t TOTAL = N * 9 / 10;
  SearchOverridesGuard ranges(TopDocsReq::boundedCountRangeDocsForTests);
  TopDocsReq::boundedCountRangeDocsForTests = DocsEnumMeta::L1_DOCS;
  CollectionHelper helper("join_ties");
  helper.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes = 0});
  CollectionHelper::UpdateBuilder input;
  for (int32_t i = 0; i < N; i++) {
    std::string json = "{\"id\":\"" + std::to_string(i) + "\",\"tag_s\":\"java\",\"answers_nested\":[";
    int32_t count = i % 127 == 0 ? 31 : 2;
    for (int32_t row = 0; row < count; row++) {
      if (row) json += ',';
      json += "{\"date_i\":" + std::string(i % 10 && row == 0 ? "0" : "100") + '}';
    }
    json += "]}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto run = [&](int64_t count, bool exactLimit = true) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("join_ties");
    auto& op = req->topDocs().getScores().limit(10).fields({"id"}).exprQuery("tag_s:java AND answers_nested.date_i:[0 TO 10]");
    if (exactLimit) op.countLimit(count);
    req->execute(1); EXPECT_OK(req);
    return req;
  };
  SkipStatsScope stats;
  auto exact = run(-1);
  ASSERT_EQ(TOTAL, exact->getMatchCount());
  auto fullWork = SkipStats::docBlocksDecoded + SkipStats::countBulkFillBlocks;
  EXPECT_GT(fullWork, 100);
  const auto ids = std::get<api::ColStr>(exact->docList()->columns.at("id").kind).v;
  const auto scores = std::get<api::ColFloat>(exact->docList()->columns.at("_score_").kind).v;
  ASSERT_EQ(10u, ids.size());
  for (size_t i = 0; i < ids.size(); i++) {
    EXPECT_EQ(std::to_string(i < 9 ? i + 1 : 11), ids[i]);
    EXPECT_EQ(scores[0], scores[i]);
  }
  for (int64_t limit : {(int64_t)0, (int64_t)1, (int64_t)10, (int64_t)10000, (int64_t)TOTAL - 1, (int64_t)TOTAL, (int64_t)TOTAL + 1, (int64_t)-1}) {
    SCOPED_TRACE(limit);
    SkipStats::reset();
    auto result = run(limit, limit != 0);
    auto names = std::get<api::ColStr>(result->docList()->columns.at("id").kind).v;
    auto values = std::get<api::ColFloat>(result->docList()->columns.at("_score_").kind).v;
    EXPECT_TRUE(std::ranges::equal(ids, names));
    EXPECT_TRUE(std::ranges::equal(scores, values));
    if (limit) {
      if (result->docList()->found_gte) { EXPECT_EQ(limit, result->getMatchCount()); }
      else { EXPECT_EQ(TOTAL, result->getMatchCount()); }
      if (limit >= TOTAL || limit < 0) { EXPECT_FALSE(result->docList()->found_gte); }
    }
    auto work = SkipStats::docBlocksDecoded + SkipStats::countBulkFillBlocks;
    if (limit >= 0 && limit <= 10000) {
      EXPECT_LT(work, fullWork / 2);
      if (limit == 0) { EXPECT_LT(work, 100); }
      else {
        EXPECT_EQ(1, SkipStats::boundedCountSegments);
        EXPECT_EQ(1, SkipStats::boundedCountStops);
      }
    }
  }
}

} // namespace
} // namespace luxir::test
