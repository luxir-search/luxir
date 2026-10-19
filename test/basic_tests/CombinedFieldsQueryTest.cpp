// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// combined_fields, multi_match and dis_max: BM25F scoring of several TEXT
// fields as one combined field, per-field types combined by dis_max, match
// semantics, request validation, and the HTTP form.

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
                         "tag_w", "Dune", "kind_s", "Dune"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "title_un", "the desert", "body_un", "dune dune sand",
                         "tag_w", "sand"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "title_un", "foundation", "body_un", "galactic empire",
                         "tag_w", "empire"), UpdateMessage::COMMIT);
  }

  // id -> score for every hit; nullopt (plus the error text) on failure
  std::optional<std::map<std::string, float>> run(const api::Query& query, bool filter = false,
                                                  std::string* error = nullptr, int limit = -1) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    if (filter) {
      cur.allQuery().filter(query);
    } else {
      cur.rawQuery() = query;
    }
    cur.fields({"id"}).limit(limit).getScores();
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

  api::Query multiMatch(std::string_view text, std::initializer_list<std::string_view> fields,
                        Type type, std::optional<float> tieBreaker = std::nullopt,
                        int32_t slop = 0) {
    auto q = qb::multiMatch(mr(), text, fields, type);
    auto& arm = std::get<api::MultiMatchQuery>(q.kind);
    arm.tie_breaker = tieBreaker;
    arm.slop = slop;
    return q;
  }

  float score(const api::Query& query, const std::string& id) {
    auto hits = run(query);
    return hits && hits->contains(id) ? hits->at(id) : 0.0f;
  }
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

  // ... multi_match cross_fields groups by analysis; groups combine as dis_max
  auto cross = run(multiMatch("Dune", {"title_un", "body_un", "tag_w"}, Type::CROSS_FIELDS));
  ASSERT_TRUE(cross);
  auto unGroup = run(qb::combinedFields(mr(), "Dune", {"title_un", "body_un"}));
  ASSERT_TRUE(unGroup);
  float tag = score(qb::match(mr(), "tag_w", "Dune"), "d1");
  ASSERT_GT(tag, 0.0f);
  EXPECT_EQ(2u, cross->size());
  EXPECT_FLOAT_EQ(std::max(unGroup->at("d1"), tag), cross->at("d1"));
  EXPECT_FLOAT_EQ(unGroup->at("d2"), cross->at("d2"));

  auto tied = run(multiMatch("Dune", {"title_un", "body_un", "tag_w"}, Type::CROSS_FIELDS, 0.5f));
  ASSERT_TRUE(tied);
  float best = std::max(unGroup->at("d1"), tag);
  EXPECT_NEAR(best + 0.5f * (unGroup->at("d1") + tag - best), tied->at("d1"), 1e-5);

  // a non-TEXT field is a group of its own
  auto keyword = run(multiMatch("Dune", {"title_un", "kind_s"}, Type::CROSS_FIELDS));
  ASSERT_TRUE(keyword);
  EXPECT_EQ(1u, keyword->size());
  EXPECT_FLOAT_EQ(std::max(score(qb::match(mr(), "title_un", "Dune"), "d1"),
                           score(qb::match(mr(), "kind_s", "Dune"), "d1")),
                  keyword->at("d1"));
}

TEST_F(CombinedFieldsQueryTest, disMaxScoresBestPlusTieBreaker) {
  auto title = run(qb::match(mr(), "title_un", "dune"));
  auto body = run(qb::match(mr(), "body_un", "dune"));
  ASSERT_TRUE(title && body);
  for (float tie : {0.0f, 0.3f}) {
    auto hits = run(qb::disMax(mr(), {qb::match(mr(), "title_un", "dune"),
                                      qb::match(mr(), "body_un", "dune")}, tie));
    ASSERT_TRUE(hits);
    ASSERT_EQ(2u, hits->size());
    for (const auto& [id, s] : *hits) {
      float t = title->contains(id) ? title->at(id) : 0.0f;
      float b = body->contains(id) ? body->at(id) : 0.0f;
      EXPECT_NEAR(std::max(t, b) + tie * std::min(t, b), s, 1e-5) << id << " tie " << tie;
    }
  }
  // top-1 collection prunes on the dis_max score bound
  auto disMax = qb::disMax(mr(), {qb::match(mr(), "title_un", "dune"),
                                  qb::match(mr(), "body_un", "dune")});
  auto all = run(disMax);
  auto top = run(disMax, false, nullptr, 1);
  ASSERT_TRUE(all && top);
  ASSERT_EQ(1u, top->size());
  auto best = std::max_element(all->begin(), all->end(),
                               [](auto& a, auto& b) { return a.second < b.second; });
  EXPECT_EQ(best->first, top->begin()->first);

  // tie_breaker 1 adds the scores like a disjunction
  auto sum = run(qb::disMax(mr(), {qb::match(mr(), "title_un", "dune"),
                                   qb::match(mr(), "body_un", "dune")}, 1.0f));
  auto most = run(multiMatch("dune", {"title_un", "body_un"}, Type::MOST_FIELDS));
  ASSERT_TRUE(sum && most);
  EXPECT_EQ(*most, *sum);

  // a two-phase disjunct, and the same match set in a filter
  auto mixed = qb::disMax(mr(), {qb::phraseText(mr(), "body_un", "desert planet"),
                                 qb::match(mr(), "title_un", "foundation")});
  auto scored = run(mixed);
  auto filtered = run(mixed, true);
  ASSERT_TRUE(scored && filtered);
  EXPECT_EQ(2u, scored->size());
  EXPECT_TRUE(scored->contains("d1") && scored->contains("d3"));
  EXPECT_EQ(2u, filtered->size());
}

TEST_F(CombinedFieldsQueryTest, bestFieldsIsDisMaxOfMatches) {
  auto expected = run(qb::disMax(mr(), {qb::match(mr(), "title_un", "dune"),
                                        qb::match(mr(), "body_un", "dune")}, 0.2f));
  auto best = run(multiMatch("dune", {"title_un", "body_un"}, Type::BEST_FIELDS, 0.2f));
  auto byDefault = run(multiMatch("dune", {"title_un", "body_un"}, Type::TYPE_UNSPECIFIED, 0.2f));
  ASSERT_TRUE(expected && best && byDefault);
  EXPECT_EQ(*expected, *best);
  EXPECT_EQ(*expected, *byDefault);

  // a field boost scales that field's score before the max
  auto boosted = run(multiMatch("dune", {"title_un^3", "body_un"}, Type::BEST_FIELDS));
  ASSERT_TRUE(boosted);
  EXPECT_NEAR(std::max(3.0f * score(qb::match(mr(), "title_un", "dune"), "d1"),
                       score(qb::match(mr(), "body_un", "dune"), "d1")),
              boosted->at("d1"), 1e-5);
}

TEST_F(CombinedFieldsQueryTest, phraseTypes) {
  auto phrase = run(multiMatch("desert planet", {"title_un", "body_un"}, Type::PHRASE));
  ASSERT_TRUE(phrase);
  EXPECT_EQ(1u, phrase->size());
  EXPECT_TRUE(phrase->contains("d1"));
  auto sloppy = run(multiMatch("desert story", {"body_un"}, Type::PHRASE, std::nullopt, 1));
  ASSERT_TRUE(sloppy);
  EXPECT_TRUE(sloppy->contains("d1"));
  auto exact = run(multiMatch("desert story", {"body_un"}, Type::PHRASE));
  ASSERT_TRUE(exact);
  EXPECT_TRUE(exact->empty());

  // phrase_prefix expands the last word; one word is a prefix over the field
  auto prefixed = run(multiMatch("desert pla", {"title_un", "body_un"}, Type::PHRASE_PREFIX));
  ASSERT_TRUE(prefixed);
  EXPECT_EQ(1u, prefixed->size());
  EXPECT_TRUE(prefixed->contains("d1"));
  auto single = run(multiMatch("des", {"title_un", "body_un"}, Type::PHRASE_PREFIX));
  ASSERT_TRUE(single);
  EXPECT_EQ(2u, single->size());
  EXPECT_TRUE(run(multiMatch("desert zzz", {"body_un"}, Type::PHRASE_PREFIX))->empty());

  // bool_prefix: leading terms by operator, the last word a prefix
  auto boolPrefix = qb::multiMatch(mr(), "sand emp", {"title_un", "body_un"}, Type::BOOL_PREFIX);
  auto any = run(boolPrefix);
  ASSERT_TRUE(any);
  EXPECT_EQ(2u, any->size());
  EXPECT_TRUE(any->contains("d2") && any->contains("d3"));
  std::get<api::MultiMatchQuery>(boolPrefix.kind).operator_ = MatchOp::AND;
  auto all = run(boolPrefix);
  ASSERT_TRUE(all);
  EXPECT_TRUE(all->empty());
}

TEST_F(CombinedFieldsQueryTest, wildcardFields) {
  auto listed = run(multiMatch("dune", {"title_un", "body_un"}, Type::BEST_FIELDS));
  auto pattern = run(multiMatch("dune", {"*_un"}, Type::BEST_FIELDS));
  ASSERT_TRUE(listed && pattern);
  EXPECT_EQ(*listed, *pattern);
  auto combined = run(qb::combinedFields(mr(), "dune", {"*_un^2"}));
  auto combinedListed = run(qb::combinedFields(mr(), "dune", {"title_un^2", "body_un^2"}));
  ASSERT_TRUE(combined && combinedListed);
  EXPECT_EQ(*combinedListed, *combined);

  // a pattern keeps TEXT and STRING fields and may select nothing
  auto broad = run(multiMatch("Dune", {"*"}, Type::BEST_FIELDS));
  ASSERT_TRUE(broad);
  EXPECT_TRUE(broad->contains("d1"));
  auto none = run(multiMatch("dune", {"nothing_*"}, Type::BEST_FIELDS));
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->empty());
}

TEST_F(CombinedFieldsQueryTest, mostFieldsAndValidation) {
  auto most = run(qb::multiMatch(mr(), "dune", {"title_un", "body_un"}, Type::MOST_FIELDS));
  ASSERT_TRUE(most);
  EXPECT_EQ(2u, most->size());

  std::string error;
  EXPECT_FALSE(run(multiMatch("dune", {"title_un"}, Type::BEST_FIELDS, 1.5f), false, &error));
  EXPECT_NE(std::string::npos, error.find("tie_breaker")) << error;
  EXPECT_FALSE(run(multiMatch("dune", {"title_un"}, Type::BEST_FIELDS, std::nullopt, 1),
                   false, &error));
  EXPECT_NE(std::string::npos, error.find("slop")) << error;
  EXPECT_FALSE(run(qb::disMax(mr(), {}), false, &error));
  EXPECT_NE(std::string::npos, error.find("non-empty")) << error;
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
  for (std::string query : {
           R"({"multi_match":{"query":"dune","fields":["title_un","body_un"],"tie_breaker":0.3}})",
           R"({"dis_max":{"queries":[{"match":{"field":"title_un","val":"dune"}},)"
           R"({"match":{"field":"body_un","val":"dune"}}],"tie_breaker":0.3}})",
           R"x("dis_max(queries=[title_un:dune, body_un:dune], tie_breaker=0.3)")x"}) {
    auto res = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
                           R"({"query":)" + query + R"(,"fields":["id"],"get_number":true})");
    EXPECT_EQ(200, res.result_int()) << query << "\n" << res.body();
    EXPECT_NE(std::string::npos, res.body().find(R"("found":2)")) << query << "\n" << res.body();
  }
  auto bad = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
      R"({"query":{"dis_max":{"queries":[{"all":true}],"tie_breaker":2}}})");
  EXPECT_EQ(400, bad.result_int()) << bad.body();
  server.shutdown();
}
