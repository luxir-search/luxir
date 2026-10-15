// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// combined_fields and multi_match: BM25F scoring of several TEXT fields as
// one combined field, match semantics, request validation, and the HTTP form.

#include <cmath>
#include <map>
#include <optional>

#include <gtest/gtest.h>

#include "luxir/search/Similarity.h"
#include "luxir/server/HttpServer.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"

using namespace luxir;
using namespace luxir::test;

using Type = api::MultiMatchQuery_::Type;
using MatchOp = api::Match_::Operator;

class CombinedFieldsQueryTest : public LuxirTest {
public:
  CollectionHelper helper{"main"};

  void SetUp() override {
    helper.index(flatdoc("id", "d1", "title_un", "dune", "body_un", "a desert planet story",
                         "tag_w", "Dune"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "title_un", "the desert", "body_un", "dune dune sand",
                         "tag_w", "sand"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "title_un", "foundation", "body_un", "galactic empire",
                         "tag_w", "empire"), UpdateMessage::COMMIT);
  }

  // id -> score for every hit; nullopt (plus the error text) on failure
  std::optional<std::map<std::string, float>> run(const api::Query& query, bool filter = false,
                                                  std::string* error = nullptr) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    if (filter) {
      cur.allQuery().filter(query);
    } else {
      cur.rawQuery() = query;
    }
    cur.fields({"id"}).limit(-1).getScores();
    req->execute();
    if (!req->ok()) {
      if (error) *error = req->errorMsg();
      return std::nullopt;
    }
    std::map<std::string, float> out;
    const auto* docs = req->docList();
    if (docs == nullptr || docs->row_count == 0) return out;
    const auto& ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
    const auto& scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    for (size_t i = 0; i < ids.size(); i++) out[std::string(ids[i])] = scores[i];
    return out;
  }

  google::protobuf::Arena protoArena;
  ArenaResource arena{&protoArena};
  std::pmr::memory_resource& mr() { return arena; }
};

// Hand-computed BM25F: title weight 2, body weight 1, one term.
TEST_F(CombinedFieldsQueryTest, scoresMatchBm25fFormula) {
  auto hits = run(qb::combinedFields(mr(), "dune", {"title_un^2", "body_un"}));
  ASSERT_TRUE(hits);
  ASSERT_EQ(2u, hits->size());

  // docCount = 3; weighted sumTotalTermFreq = 2*4 + 9; df = max(1, 1)
  Similarity sim;
  float idf = sim.idf(1, 3);
  float avgdl = (float)(17.0 / 3.0);
  auto bm25f = [&](float freq, float length) {
    return Similarity::BM25Scorer(1.0f, sim.k1, sim.b, idf, avgdl)
        .score(freq, SmallFloat::intToByte4((int32_t)length));
  };
  // d1: freq 2*1, length 2*1 + 4; d2: freq 1*2, length 2*2 + 3
  EXPECT_NEAR(bm25f(2.0f, 6.0f), hits->at("d1"), 1e-6);
  EXPECT_NEAR(bm25f(2.0f, 7.0f), hits->at("d2"), 1e-6);
  EXPECT_GT(hits->at("d1"), hits->at("d2"));

  // multi_match cross_fields over the same fields is the same query
  auto cross = run(qb::multiMatch(mr(), "dune", {"title_un^2", "body_un"}, Type::CROSS_FIELDS));
  ASSERT_TRUE(cross);
  EXPECT_EQ(*hits, *cross);
}

TEST_F(CombinedFieldsQueryTest, operatorAndFilterContext) {
  // AND: every term somewhere in the combined field
  auto both = run(qb::combinedFields(mr(), "dune desert", {"title_un", "body_un"}, MatchOp::AND));
  ASSERT_TRUE(both);
  EXPECT_EQ(2u, both->size());
  EXPECT_TRUE(both->contains("d1") && both->contains("d2"));

  auto any = run(qb::combinedFields(mr(), "dune empire", {"title_un", "body_un"}));
  ASSERT_TRUE(any);
  EXPECT_EQ(3u, any->size());

  // unscored filter use keeps the same match set
  auto filtered = run(qb::combinedFields(mr(), "dune empire", {"title_un", "body_un"}), true);
  ASSERT_TRUE(filtered);
  EXPECT_EQ(3u, filtered->size());
}

TEST_F(CombinedFieldsQueryTest, analysisGroups) {
  std::string error;
  // _w keeps "Dune" while _un folds it: combined_fields refuses the mix ...
  EXPECT_FALSE(run(qb::combinedFields(mr(), "Dune", {"title_un", "tag_w"}), false, &error));
  EXPECT_NE(std::string::npos, error.find("analyze the query text differently")) << error;

  // ... multi_match cross_fields groups by analysis and adds the groups
  auto cross = run(qb::multiMatch(mr(), "Dune", {"title_un", "body_un", "tag_w"}, Type::CROSS_FIELDS));
  ASSERT_TRUE(cross);
  auto unGroup = run(qb::combinedFields(mr(), "Dune", {"title_un", "body_un"}));
  ASSERT_TRUE(unGroup);
  EXPECT_EQ(2u, cross->size());
  EXPECT_GT(cross->at("d1"), unGroup->at("d1"));  // tag_w:Dune adds to d1 only
  EXPECT_FLOAT_EQ(cross->at("d2"), unGroup->at("d2"));
}

TEST_F(CombinedFieldsQueryTest, mostFieldsAndValidation) {
  auto most = run(qb::multiMatch(mr(), "dune", {"title_un", "body_un"}, Type::MOST_FIELDS));
  ASSERT_TRUE(most);
  EXPECT_EQ(2u, most->size());

  std::string error;
  EXPECT_FALSE(run(qb::multiMatch(mr(), "dune", {"title_un"}, Type::TYPE_UNSPECIFIED), false, &error));
  EXPECT_NE(std::string::npos, error.find("best_fields")) << error;
  EXPECT_FALSE(run(qb::combinedFields(mr(), "dune", {"title_un^0.5"}), false, &error));
  EXPECT_NE(std::string::npos, error.find(">= 1")) << error;
  EXPECT_FALSE(run(qb::combinedFields(mr(), "dune", {"title_un^x"}), false, &error));
  EXPECT_FALSE(run(qb::combinedFields(mr(), "dune", {"id"}), false, &error));
  EXPECT_NE(std::string::npos, error.find("not a TEXT field")) << error;
}

TEST_F(CombinedFieldsQueryTest, httpJsonAndExpr) {
  HttpServer server(*LuxirTest::luxirNode, 1, 0);
  server.start();
  for (std::string query : {
           R"({"combined_fields":{"query":"dune","fields":["title_un^2","body_un"]}})",
           R"({"multi_match":{"query":"dune desert","fields":["title_un","body_un"],)"
           R"("type":"cross_fields","operator":"and"}})",
           R"x("combined_fields(dune, fields=[title_un, body_un])")x"}) {
    auto res = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
                           R"({"query":)" + query + R"(,"fields":["id"],"get_number":true})");
    EXPECT_EQ(200, res.result_int()) << query << "\n" << res.body();
    EXPECT_NE(std::string::npos, res.body().find(R"("found":2)")) << query << "\n" << res.body();
  }
  auto bad = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
      R"({"query":{"multi_match":{"query":"dune","fields":["title_un"],"type":"phrase"}}})");
  EXPECT_EQ(400, bad.result_int()) << bad.body();
  server.shutdown();
}
