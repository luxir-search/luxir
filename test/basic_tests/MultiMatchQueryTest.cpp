// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// multi_match: BM25F scoring of several TEXT fields as
// one combined field, match semantics, request validation, and the HTTP form.

#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <typeindex>

#include <gtest/gtest.h>

#include "luxir/query/ParseContext.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/search/Similarity.h"
#include "luxir/server/HttpServer.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

using Mode = api::MultiMatchQuery_::Mode;
using MatchOp = api::Match_::Operator;

class MultiMatchQueryTest : public LuxirTest {
public:
  CollectionHelper helper{"main"};
  google::protobuf::Arena protoArena;
  ArenaResource arena{&protoArena};
  MemPool parsePool;

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

  std::pmr::memory_resource& mr() { return arena; }

  Query* parse(const api::Query& query) {
    ParseContext context{parsePool, *helper.collection().getSchema(), protoArena, CoerceContext{}, ""};
    return ProtobufQueryParser(context).parse(query);
  }

  static FilterKey filterKey(const Query& query) {
    FilterKeyContext context;
    FilterKeyBuilder builder;
    auto scope = query.appendFilterKey(builder, context);
    return std::move(builder).finishStructural(scope, context);
  }
};

// Hand-computed BM25F: title weight 2, body weight 1, one term.
TEST_F(MultiMatchQueryTest, scoresMatchBm25fFormula) {
  auto hits = run(qb::multiMatch(mr(), "dune", {{"title_un", 2.0f}, {"body_un", {}}}));
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
}

// title_un in 1 of 3 docs is under half density, so its norms are sparse and
// a doc without a title adds no title length.
class MultiMatchSparseNormsTest : public MultiMatchQueryTest {
public:
  void SetUp() override {
    helper.index(flatdoc("id", "s1", "title_un", "dune", "body_un", "dune sand"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "s2", "body_un", "dune desert planet"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "s3", "body_un", "galactic empire"), UpdateMessage::COMMIT);
  }
};

TEST_F(MultiMatchSparseNormsTest, missingFieldAddsNoLength) {
  auto hits = run(qb::multiMatch(mr(), "dune", {{"title_un", 2.0f}, {"body_un", {}}}));
  ASSERT_TRUE(hits);
  ASSERT_EQ(2u, hits->size());

  // docCount = max(1, 3); weighted sumTotalTermFreq = 2*1 + 7; df = max(1, 2)
  Similarity sim;
  float idf = sim.idf(2, 3);
  float avgdl = (float)(9.0 / 3.0);
  auto bm25f = [&](float freq, float length) {
    return Similarity::BM25Scorer(1.0f, sim.k1, sim.b, idf, avgdl)
        .score(freq, SmallFloat::intToByte4((int32_t)length));
  };
  // s1: freq 2*1 + 1, length 2*1 + 2; s2: freq 1, length 3 (no title)
  EXPECT_NEAR(bm25f(3.0f, 4.0f), hits->at("s1"), 1e-6);
  EXPECT_NEAR(bm25f(1.0f, 3.0f), hits->at("s2"), 1e-6);
}

TEST_F(MultiMatchQueryTest, operatorAndFilterContext) {
  // AND: every term somewhere in the combined field
  auto both = run(qb::multiMatch(mr(), "dune desert",
      {{"title_un", {}}, {"body_un", {}}, {"tag_w", {}}}, Mode::COMBINED, MatchOp::AND));
  ASSERT_TRUE(both);
  EXPECT_EQ(2u, both->size());
  EXPECT_TRUE(both->contains("d1") && both->contains("d2"));

  auto any = run(qb::multiMatch(mr(), "dune empire", {{"title_un", {}}, {"body_un", {}}}));
  ASSERT_TRUE(any);
  EXPECT_EQ(3u, any->size());

  // unscored filter use keeps the same match set
  auto filtered = run(qb::multiMatch(mr(), "dune empire", {{"title_un", {}}, {"body_un", {}}}), true);
  ASSERT_TRUE(filtered);
  ASSERT_EQ(any->size(), filtered->size());
  for (const auto& [id, score] : *any) EXPECT_TRUE(filtered->contains(id));
}

TEST_F(MultiMatchQueryTest, analysisGroups) {
  // Analyzer identity determines groups even when the tokens coincide.
  std::string_view names[] = {"body_un", "title_un"};
  float weights[] = {1.0f, 1.0f};
  for (std::string_view text : {"dune", "Dune"}) {
    auto* query = parse(qb::multiMatch(mr(), text,
        {{"title_un", {}}, {"body_un", {}}, {"tag_w", {}}}));
    CombinedFieldQuery combined(names, weights, "dune");
    TermQuery tag("tag_w", text);
    Query* optional[] = {&combined, &tag};
    BooleanQuery expected({}, optional, {}, {});
    EXPECT_TRUE(query->equals(expected));
  }

  // The independent whitespace group adds to d1 only for uppercase Dune.
  auto cross = run(qb::multiMatch(mr(), "Dune", {{"title_un", {}}, {"body_un", {}}, {"tag_w", {}}}, Mode::COMBINED));
  ASSERT_TRUE(cross);
  auto unGroup = run(qb::multiMatch(mr(), "Dune", {{"title_un", {}}, {"body_un", {}}}));
  ASSERT_TRUE(unGroup);
  EXPECT_EQ(2u, cross->size());
  EXPECT_GT(cross->at("d1"), unGroup->at("d1"));  // tag_w:Dune adds to d1 only
  EXPECT_FLOAT_EQ(cross->at("d2"), unGroup->at("d2"));
}

TEST_F(MultiMatchQueryTest, longTermsPolicySplitsGroups) {
  SchemaBuilder schema;
  schema.field("a").parent = "_un";
  auto& b = schema.field("b");
  b.parent = "_un";
  b.long_terms = api::FieldDef::LongTerms::TRUNCATE;
  auto compiled = schema.set(helper.collection());
  auto* aType = (TextFieldType*)compiled->physical("a");
  auto* bType = (TextFieldType*)compiled->physical("b");
  ASSERT_EQ(aType->analyzer_.get(), bType->analyzer_.get());
  ASSERT_NE(aType->longTerms, bType->longTerms);

  QueryBuilder builder(parsePool, *compiled, CoerceContext{});
  for (std::string text : {std::string("dune"), std::string(PackedTerm::MAX_LEN + 1, 'a')}) {
    Query* optional[] = {builder.createMatchQuery("a", text), builder.createMatchQuery("b", text)};
    BooleanQuery expected({}, optional, {}, {});
    auto* query = parse(qb::multiMatch(mr(), text, {{"b", {}}, {"a", {}}}));
    EXPECT_TRUE(query->equals(expected));
  }
}

TEST_F(MultiMatchQueryTest, fieldOrderBindingsAndDuplicates) {
  SchemaBuilder schema;
  auto& name = schema.field("name");
  name.type = api::FieldDef::FieldClass::STRING;
  schema.variant(name, "t").parent = "_un";
  name.defaults.emplace().search = "t";
  schema.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "d4", "name", "dune", "tag_w", "dune",
                                  "body_un", "empire"), UpdateMessage::COMMIT).success);

  for (auto mode : {Mode::COMBINED, Mode::PER_FIELD}) {
    auto first = qb::multiMatch(mr(), "dune",
        {{"title_un", 2.0f}, {"tag_w", 0.5f}, {"name", 1.0f}, {"body_un", {}},
         {"name__t", 3.0f}, {"title_un", 1.0f}}, mode);
    auto second = qb::multiMatch(mr(), "dune",
        {{"name__t", 3.0f}, {"body_un", {}}, {"tag_w", 0.5f}, {"title_un", 2.0f}}, mode);
    auto* a = parse(first);
    auto* b = parse(second);
    EXPECT_TRUE(a->equals(*b));
    EXPECT_EQ(a->hash(), b->hash());
    EXPECT_EQ(filterKey(*a), filterKey(*b));
    auto aHits = run(first);
    auto bHits = run(second);
    ASSERT_TRUE(aHits && bHits);
    EXPECT_EQ(*aHits, *bHits);
  }
}

TEST_F(MultiMatchQueryTest, emptyAnalysisGroupWithAndAndMinMatch) {
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "p1", "tag_w", "!!! ??? ###"),
      flatdoc("id", "p2", "tag_w", "!!! ???"),
      flatdoc("id", "p3", "tag_w", "!!!")}, UpdateMessage::COMMIT).success);
  auto query = qb::multiMatch(mr(), "!!! ??? ###",
      {{"title_un", {}}, {"tag_w", {}}, {"body_un", {}}}, Mode::COMBINED, MatchOp::AND);
  auto all = run(query);
  ASSERT_TRUE(all);
  ASSERT_EQ(1u, all->size());
  EXPECT_TRUE(all->contains("p1"));
  auto& multi = std::get<api::MultiMatchQuery>(query.kind);
  multi.operator_ = MatchOp::OR;
  multi.min_match = 2;
  auto two = run(query);
  ASSERT_TRUE(two);
  EXPECT_EQ(2u, two->size());
  EXPECT_TRUE(two->contains("p1") && two->contains("p2"));

  auto empty = qb::multiMatch(mr(), "!!! ??? ###", {{"title_un", {}}, {"body_un", {}}});
  EXPECT_EQ(QueryKind::NONE, parse(empty)->getKind());
  auto none = run(empty);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->empty());
}

TEST_F(MultiMatchQueryTest, perFieldUsesBoostedTermLeaves) {
  for (auto [field, weight] : {std::pair{"title_un", 2.0f}, {"body_un", 0.5f}}) {
    auto query = qb::multiMatch(mr(), "dune", {{field, weight}}, Mode::PER_FIELD);
    auto* term = dynamic_cast<TermQuery*>(parse(query));
    ASSERT_NE(nullptr, term);
    EXPECT_EQ(field, term->getField());
    EXPECT_FLOAT_EQ(weight, term->getBoost());
    auto weighted = run(query);
    auto ordinary = run(qb::match(mr(), field, "dune"));
    ASSERT_TRUE(weighted && ordinary);
    ASSERT_EQ(ordinary->size(), weighted->size());
    for (const auto& [id, score] : *ordinary) {
      EXPECT_FLOAT_EQ(weight * score, weighted->at(id));
    }
  }
  auto* boolean = dynamic_cast<BooleanQuery*>(parse(qb::multiMatch(mr(), "dune",
      {{"title_un", 2.0f}, {"body_un", 0.5f}}, Mode::PER_FIELD)));
  ASSERT_NE(nullptr, boolean);
  auto shape = boolean->normalizationForTest(parsePool);
  ASSERT_EQ(2u, shape.optionalTypes.size());
  for (auto type : shape.optionalTypes) EXPECT_EQ(std::type_index(typeid(TermQuery)), type);
}

TEST_F(MultiMatchQueryTest, perFieldCoercesNumericMatch) {
  ASSERT_TRUE(helper.index(flatdoc("id", "number", "count_i", 7), UpdateMessage::COMMIT).success);
  auto query = qb::multiMatch(mr(), "7", {{"count_i", 2.0f}}, Mode::PER_FIELD);
  auto* boost = dynamic_cast<BoostQuery*>(parse(query));
  ASSERT_NE(nullptr, boost);
  EXPECT_EQ(QueryKind::NUMERIC_PREDICATE, boost->getChild()->getKind());
  auto weighted = run(query);
  auto ordinary = run(qb::match(mr(), "count_i", "7"));
  ASSERT_TRUE(weighted && ordinary);
  ASSERT_EQ(1u, weighted->size());
  EXPECT_FLOAT_EQ(2.0f * ordinary->at("number"), weighted->at("number"));
}

TEST_F(MultiMatchQueryTest, singletonCombinedKeepsBm25fWeights) {
  for (float weight : {0.5f, 1.0f, 2.0f}) {
    auto query = qb::multiMatch(mr(), "dune", {{"title_un", weight}});
    auto* built = parse(query);
    if (weight == 1.0f) {
      EXPECT_EQ(QueryKind::TERM, built->getKind());
    } else {
      auto* combined = dynamic_cast<CombinedFieldQuery*>(built);
      ASSERT_NE(nullptr, combined);
      ASSERT_EQ(1u, combined->getFields().size());
      EXPECT_FLOAT_EQ(weight, combined->getWeights()[0]);
    }
    auto hits = run(query);
    ASSERT_TRUE(hits);
    ASSERT_EQ(1u, hits->size());
    Similarity sim;
    auto scorer = Similarity::BM25Scorer(1.0f, sim.k1, sim.b, sim.idf(1, 3), weight * 4.0f / 3.0f);
    EXPECT_NEAR(scorer.score(weight, SmallFloat::intToByte4((int32_t)std::round(weight))),
                hits->at("d1"), 1e-6);
  }
}

TEST_F(MultiMatchQueryTest, finiteExtremeWeightsKeepMembershipAndScores) {
  for (float weight : {std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max()}) {
    auto query = qb::multiMatch(mr(), "dune", {{"title_un", weight}, {"body_un", weight}});
    auto hits = run(query);
    auto filtered = run(query, true);
    ASSERT_TRUE(hits && filtered);
    ASSERT_EQ(2u, hits->size());
    EXPECT_EQ(hits->size(), filtered->size());
    for (const auto& [id, score] : *hits) {
      EXPECT_TRUE(std::isfinite(score));
      EXPECT_GE(score, 0.0f);
      EXPECT_TRUE(filtered->contains(id));
    }
  }
}

TEST_F(MultiMatchQueryTest, modesAndWeightValidation) {
  for (auto mode : {Mode::COMBINED, Mode::PER_FIELD}) {
    auto hits = run(qb::multiMatch(mr(), "dune", {{"title_un", 0.5f}, {"body_un", {}}}, mode));
    ASSERT_TRUE(hits);
    EXPECT_EQ(2u, hits->size());
    for (float weight : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity()}) {
      std::string error;
      EXPECT_FALSE(run(qb::multiMatch(mr(), "dune", {{"title_un", weight}}, mode), false, &error));
      EXPECT_NE(std::string::npos, error.find("field 'title_un'")) << error;
      EXPECT_NE(std::string::npos, error.find("finite and > 0 (got ")) << error;
    }
  }
  std::string error;
  EXPECT_FALSE(run(qb::multiMatch(mr(), "dune", {}), false, &error));
  EXPECT_NE(std::string::npos, error.find("non-empty 'fields'")) << error;
  EXPECT_FALSE(run(qb::multiMatch(mr(), "dune", {{"", {}}}), false, &error));
  EXPECT_NE(std::string::npos, error.find("empty field name")) << error;
  auto negative = qb::multiMatch(mr(), "dune", {{"title_un", {}}});
  std::get<api::MultiMatchQuery>(negative.kind).min_match = -1;
  EXPECT_FALSE(run(negative, false, &error));
  EXPECT_NE(std::string::npos, error.find("min_match")) << error;
  EXPECT_FALSE(run(qb::multiMatch(mr(), "dune", {{"id", {}}}), false, &error));
  EXPECT_NE(std::string::npos, error.find("not a TEXT field")) << error;
  EXPECT_FALSE(run(qb::multiMatch(mr(), "dune", {{"title_un", {}}}, (Mode)123), false, &error));
  EXPECT_NE(std::string::npos, error.find("unknown mode 123")) << error;
}

TEST_F(MultiMatchQueryTest, httpJsonAndExpr) {
  HttpServer server(*LuxirTest::luxirNode, 1, 0);
  server.start();
  for (std::string query : {
           R"({"multi_match":{"text":"dune","fields":["title_un^2","body_un"]}})",
           R"({"multi_match":{"text":"dune desert","fields":[{"field":"title_un"},{"field":"body_un"}],)"
           R"("mode":"combined","operator":"and"}})",
           R"x("multi_match(dune, fields=[title_un^2, body_un], mode=per_field)")x"}) {
    auto res = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
                           R"({"query":)" + query + R"(,"fields":["id"],"count_limit":-1})");
    EXPECT_EQ(200, res.result_int()) << query << "\n" << res.body();
    EXPECT_NE(std::string::npos, res.body().find(R"("found":2)")) << query << "\n" << res.body();
  }
  auto bad = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
      R"({"query":{"combined_fields":{"query":"dune","fields":["title_un"]}}})");
  EXPECT_EQ(400, bad.result_int()) << bad.body();
  EXPECT_NE(std::string::npos, bad.body().find("unknown_key")) << bad.body();
  server.shutdown();
}
