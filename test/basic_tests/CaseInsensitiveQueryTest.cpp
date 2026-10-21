// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/schema/Schema.h"

using namespace luxir;
using namespace luxir::test;

// case_insensitive over fields that keep case:
// name_s is a STRING without a normalizer, body_w a whitespace TEXT field.
class CaseInsensitiveQueryTest : public LuxirTest {
protected:
  CollectionHelper helper{"main"};
  using Ids = std::vector<std::string>;

  void SetUp() override {
    ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a", "name_s", "FOO", "body_w", "Error HAPPENED", "title_t", "Error"),
      flatdoc("id", "b", "name_s", "foo", "body_w", "error happened", "title_t", "error"),
      flatdoc("id", "c", "name_s", "Foo", "body_w", "ERROR"),
      flatdoc("id", "d", "name_s", "bar", "body_w", "other"),
      flatdoc("id", "e", "name_s", "\u212a", "body_w", "other"),  // KELVIN SIGN
    }, UpdateMessage::COMMIT).success);
  }

  LocalReqHandle run(std::string_view json) {
    auto req = localReq(helper.getSearchEngine());
    auto kept = api::build::arenaStr(req->mr, json);
    parseQueryRequest(kept, req->rawRequest(), req->mr);
    req->collection("main").execute(false);
    return req;
  }

  static Ids ids(const LocalReq& req) {
    EXPECT_TRUE(req.ok()) << req.errorMsg();
    Ids out;
    for (const auto& doc : req.getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(out.begin(), out.end());
    return out;
  }

  Ids hits(std::string_view query) {
    return ids(*run(std::format(R"({{"query":{},"fields":["id"],"limit":-1}})", query)));
  }

  Ids expr(std::string_view expression) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(expression).fields({"id"}).limit(-1);
    req->execute(false);
    return ids(*req);
  }
};

TEST_F(CaseInsensitiveQueryTest, stringLiteralsIgnoreCase) {
  for (auto query : {
      R"({"wildcard":{"field":"name_s","pattern":"fO?","case_insensitive":true}})",
      R"({"regex":{"field":"name_s","pattern":"fo+","case_insensitive":true}})",
      R"({"match":{"name_s":"fOO","case_insensitive":true}})",
      R"({"match":{"field":"name_s","val":"FOO","case_insensitive":true}})"}) {
    EXPECT_EQ((Ids{"a", "b", "c"}), hits(query)) << query;
  }
  EXPECT_EQ((Ids{"b"}), hits(R"({"wildcard":{"field":"name_s","pattern":"fo?"}})"));
  EXPECT_EQ((Ids{"c"}), hits(R"({"match":{"name_s":"Foo"}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}), expr("wildcard(f*, field=name_s, case_insensitive=true)"));
  EXPECT_EQ((Ids{"a", "b", "c"}), expr("match(foo, field=name_s, case_insensitive=true)"));
  // Escaped literal: '*' in a case-insensitive Match is not a wildcard.
  EXPECT_TRUE(hits(R"({"match":{"name_s":"f*","case_insensitive":true}})").empty());
  // Simple case folding classes span more than lower/upper.
  EXPECT_EQ((Ids{"e"}), hits(R"({"match":{"name_s":"k","case_insensitive":true}})"));
}

TEST_F(CaseInsensitiveQueryTest, regexClassesExpandCase) {
  EXPECT_EQ((Ids{"b"}), hits(R"({"regex":{"field":"name_s","pattern":"[a-f]oo"}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}),
            hits(R"({"regex":{"field":"name_s","pattern":"[a-f]oo","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"b"}), hits(R"({"regex":{"field":"name_s","pattern":"[^F]oo"}})"));
  EXPECT_TRUE(hits(R"({"regex":{"field":"name_s","pattern":"[^F][oO]+","case_insensitive":true}})").empty());
  EXPECT_EQ((Ids{"e"}), hits(R"({"regex":{"field":"name_s","pattern":"[j-l]","case_insensitive":true}})"));
}

TEST_F(CaseInsensitiveQueryTest, textMatchUsesItsAnalyzer) {
  EXPECT_EQ((Ids{"b"}), hits(R"({"match":{"body_w":"error"}})"));
  EXPECT_EQ((Ids{"a", "b"}), hits(R"({"match":{"title_t":"ERROR"}})"));
  for (auto field : {"body_w", "title_t"}) {
    auto req = run(std::format(R"({{"query":{{"match":{{"field":"{}","val":"error","case_insensitive":true}}}}}})", field));
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("folding"));
  }
  auto req = localReq(helper.getSearchEngine());
  req->topDocs("q").exprQuery("match(error, field=body_w, case_insensitive=true)");
  req->execute(false);
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("folding"));
}

TEST_F(CaseInsensitiveQueryTest, idAndNumericMatch) {
  EXPECT_EQ((Ids{"a"}), hits(R"({"match":{"id":"A","case_insensitive":true}})"));
  helper.index(flatdoc("id", "num", "count_i", 7, "when_dt", "2026-10-21"), UpdateMessage::COMMIT);
  for (auto query : {
      R"({"match":{"field":"count_i","val":7,"case_insensitive":true}})",
      R"({"match":{"field":"count_i","val":"7","case_insensitive":true}})",
      R"({"match":{"field":"when_dt","val":"2026-10-21","case_insensitive":true}})"}) {
    EXPECT_EQ((Ids{"num"}), hits(query));
  }
}

TEST_F(CaseInsensitiveQueryTest, prefixIgnoresCase) {
  EXPECT_EQ((Ids{"a", "b", "c"}),
      hits(R"({"prefix":{"field":"name_s","prefix":"fO","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"b"}), hits(R"({"prefix":{"field":"name_s","prefix":"fo"}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}), expr("prefix(fO, field=name_s, case_insensitive=true)"));
  EXPECT_EQ((Ids{"a", "b", "c"}),
      hits(R"({"prefix":{"field":"body_w","prefix":"err","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"b"}), hits(R"({"prefix":{"field":"body_w","prefix":"err"}})"));
  EXPECT_EQ((Ids{"a"}), hits(R"({"prefix":{"field":"id","prefix":"A","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"e"}), hits(R"({"prefix":{"field":"name_s","prefix":"k","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b", "c", "d", "e"}),
      hits(R"({"prefix":{"field":"name_s","prefix":"","case_insensitive":true}})"));
}

TEST_F(CaseInsensitiveQueryTest, prefixEscapesLiteralSyntax) {
  ASSERT_TRUE(helper.index(flatdoc("id", "escaped", "name_s", "F*?\\Tail"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((Ids{"escaped"}),
      hits(R"({"prefix":{"field":"name_s","prefix":"f*?\\","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"escaped"}), expr(R"(prefix("f*?\\", field=name_s, case_insensitive=true))"));
}

TEST_F(CaseInsensitiveQueryTest, flagSeparatesQueryIdentity) {
  // Same pattern with and without the flag: neither boolean dedup nor the
  // filter cache may merge them.
  EXPECT_EQ((Ids{"a", "b", "c"}), hits(R"({"boolean":{"optional":[
      {"regex":{"field":"name_s","pattern":"Foo"}},
      {"regex":{"field":"name_s","pattern":"Foo","case_insensitive":true}}]}})"));
  auto req = run(R"json({"query":{"all":true},"limit":0,"ops":{"f":{"query_facet":{"buckets":{
      "exact":"wildcard(Fo*, field=name_s)",
      "ci":"wildcard(Fo*, field=name_s, case_insensitive=true)",
      "again":"wildcard(Fo*, field=name_s)"}}}}})json");
  ASSERT_OK(req);
  const auto& facet = *req->docList()->ops.at("f")->facetResult();
  EXPECT_EQ((std::vector<int64_t>{1, 3, 1}), std::vector<int64_t>(facet.counts.begin(), facet.counts.end()));
}

class CaseInsensitiveMappingTest : public CaseInsensitiveQueryTest {
protected:
  void SetUp() override {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    ASSERT_TRUE(api::read_json(def, R"({"fields":{"id":"id","body":{"type":"text",
      "analyzer":{"tokenizer":"whitespace","char_filters":[{"name":"mapping",
        "params":{"mappings":["a => b","b => c","ph => f"]}}]}}}})", arena));
    helper.collection().setSchema(Schema::fromProto(def));
    ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a", "body", "apple"), flatdoc("id", "b", "body", "apricot"),
      flatdoc("id", "c", "body", "berry"), flatdoc("id", "d", "body", "BERRY"),
      flatdoc("id", "e", "body", "phone"),
    }, UpdateMessage::COMMIT).success);
  }
};

TEST_F(CaseInsensitiveMappingTest, prefixNormalizesOnce) {
  EXPECT_EQ((Ids{"a", "b", "d"}),
      hits(R"({"prefix":{"field":"body","prefix":"a","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b", "d"}), expr("prefix(a, field=body, case_insensitive=true)"));
  EXPECT_EQ((Ids{"a", "b", "d"}),
      hits(R"({"wildcard":{"field":"body","pattern":"a*","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"e"}),
      hits(R"({"prefix":{"field":"body","prefix":"ph","case_insensitive":true}})"));
}
