// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "luxir/server/JsonRequest.h"

using namespace luxir;
using namespace luxir::test;

// case_insensitive on wildcard, regex and match over fields that keep case:
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

TEST_F(CaseInsensitiveQueryTest, textMatchIgnoresCase) {
  EXPECT_EQ((Ids{"b"}), hits(R"({"match":{"body_w":"error"}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}), hits(R"({"match":{"body_w":"error","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b"}),
            hits(R"({"match":{"body_w":"ERROR Happened","operator":"and","case_insensitive":true}})"));
  // A folding analyzer already erases case: the flag keeps the scored term query.
  EXPECT_EQ((Ids{"a", "b"}), hits(R"({"match":{"title_t":"ERROR","case_insensitive":true}})"));
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
