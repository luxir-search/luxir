// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "luxir/server/JsonRequest.h"

using namespace luxir;
using namespace luxir::test;

class FieldVariantsQueryTest : public LuxirTest {
protected:
  CollectionHelper helper{"main"};

  void SetUp() override {
    SchemaBuilder b;
    auto& author = b.field("author");
    author.type = api::FieldDef::FieldClass::TEXT;
    b.analyzer(author, "unicode_word", {"nfkc_cf"});
    auto& whole = b.variant(author, "s");
    whole.type = api::FieldDef::FieldClass::STRING;
    b.normalizer(whole, {"nfkc_cf", "fold"});
    author.defaults.emplace().value = "s";

    auto& title = b.field("title");
    title.type = api::FieldDef::FieldClass::TEXT;
    b.analyzer(title, "unicode_word", {"nfkc_cf", "fold"});
    auto& preserve = b.variant(title, "preserve");
    preserve.type = api::FieldDef::FieldClass::TEXT;
    b.analyzer(preserve, "unicode_word", {"nfkc_cf"});

    auto& edition = b.field("edition");
    edition.type = api::FieldDef::FieldClass::INT;
    edition.index = api::FieldDef::IndexMode::RANGE;
    b.variant(edition, "label").type = api::FieldDef::FieldClass::STRING;
    // Deliberately different bindings: numeric match must not rebind as VALUE.
    edition.defaults.emplace().value = "label";

    auto& genre = b.field("genre");
    genre.type = api::FieldDef::FieldClass::STRING;
    auto& text = b.variant(genre, "t");
    text.type = api::FieldDef::FieldClass::TEXT;
    b.analyzer(text, "unicode_word", {"nfkc_cf"});
    genre.defaults.emplace().search = "t";

    auto& dynamic = b.templ("_number");
    dynamic.type = api::FieldDef::FieldClass::TEXT;
    b.variant(dynamic, "s").type = api::FieldDef::FieldClass::INT;
    dynamic.defaults.emplace().value = "s";
    b.set(helper.collection());

    ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a", "author", "Ursula K. Le Guin", "title", "Caf\u00e9 Stories",
              "edition", "0042", "genre", "Science Fiction", "book_number", "0042"),
      flatdoc("id", "b", "author", "George R.R. Martin", "title", "Cafe Tales",
              "edition", "10", "genre", "Historical Fiction", "book_number", "10"),
      flatdoc("id", "c", "author", "Ursula K. Le Guin", "title", "Other Stories",
              "edition", "2", "genre", "Science Fantasy", "book_number", "2"),
      flatdoc("id", "d", "author", "Le Carr\u00e9", "title", "Other Tales",
              "edition", "03", "genre", "Literary Fiction", "book_number", "03"),
    }, UpdateMessage::COMMIT).success);
  }

  LocalReqHandle run(std::string_view json) {
    auto req = localReq(helper.getSearchEngine());
    auto kept = api::build::arenaStr(req->mr, json);
    parseQueryRequest(kept, req->rawRequest(), req->mr);
    req->collection("main").execute(false);
    return req;
  }

  static std::vector<std::string> ids(const LocalReq& req, bool ordered = false) {
    EXPECT_TRUE(req.ok()) << req.errorMsg();
    std::vector<std::string> out;
    for (const auto& doc : req.getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    if (!ordered) std::sort(out.begin(), out.end());
    return out;
  }

  std::vector<std::string> hits(std::string_view query) {
    auto req = run(std::format(R"({{"query":{},"fields":["id"],"limit":-1}})", query));
    return ids(*req);
  }

  std::vector<std::string> expr(std::string_view expression) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(expression).fields({"id"}).limit(-1);
    req->execute(false);
    return ids(*req);
  }

  static std::vector<std::pair<std::string, int64_t>> buckets(const api::FacetResult& facet) {
    std::vector<std::pair<std::string, int64_t>> result;
    auto names = std::get<api::ColStr>(facet.bucket_ids->kind).v;
    for (size_t i = 0; i < names.size(); i++) result.emplace_back(names[i], facet.counts[i]);
    std::sort(result.begin(), result.end());
    return result;
  }
};

TEST_F(FieldVariantsQueryTest, authorSearchAndValueOperations) {
  EXPECT_EQ((std::vector<std::string>{"a", "c", "d"}), hits(R"({"match":{"author":"Le"}})"));
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), hits(R"({"any_of":{"field":"author","values":["URSULA K. LE GUIN"]}})"));
  EXPECT_TRUE(hits(R"({"any_of":{"field":"author","values":["Le"]}})").empty());
  EXPECT_EQ((std::vector<std::string>{"b", "d"}), hits(R"({"range":{"field":"author","lte":"M"}})"));
  auto filter = run(R"({"query":{"all":true},"filter":[{"match":{"author":"Le"}}],"fields":["id"],"limit":-1})");
  EXPECT_EQ((std::vector<std::string>{"a", "c", "d"}), ids(*filter));

  auto req = run(R"({"query":{"all":true},"fields":["id"],"limit":-1,"sort":[{"expr":"author"},{"expr":"id"}],"ops":{
    "authors":{"field_facet":{"field":"author","limit":-1}},
    "tokens":{"field_facet":{"field":"author__self","limit":-1}}
  }})");
  EXPECT_EQ((std::vector<std::string>{"b", "d", "a", "c"}), ids(*req, true));
  auto* docs = req->docList();
  ASSERT_NE(nullptr, docs);
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
      {"george r.r. martin", 1}, {"le carre", 1}, {"ursula k. le guin", 2}}),
      buckets(*docs->ops.at("authors")->facetResult()));
  auto tokens = buckets(*docs->ops.at("tokens")->facetResult());
  EXPECT_TRUE(std::ranges::contains(tokens, std::pair<std::string, int64_t>{"le", 3}));
}

TEST_F(FieldVariantsQueryTest, defaultNameTemplatesSearchFacetAndSort) {
  helper.clear();
  const std::string george = "George R.R. Martin";
  const std::string andre = "Andr\u00e9 Winters";
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "a", "author_name", george, "contributor_names", vecs(george)),
    flatdoc("id", "b", "author_name", andre, "contributor_names", vecs(andre, george)),
    flatdoc("id", "c", "author_name", "Winter's", "contributor_names", vecs("Winter's")),
  }, UpdateMessage::COMMIT).success);

  EXPECT_EQ(vecs("a"), expr("author_name:(george AND martin)"));
  EXPECT_EQ(vecs("a", "b"), expr("contributor_names:(george AND martin)"));
  for (auto field : {"author_name", "contributor_names"}) {
    SCOPED_TRACE(field);
    EXPECT_EQ(vecs("b"), expr(std::string(field) + ":ANDRE"));
    EXPECT_EQ(vecs("b"), expr(std::string(field) + ":winters"));
    EXPECT_TRUE(expr(std::string(field) + ":winter").empty());
    EXPECT_EQ(vecs("c"), expr(std::string(field) + ":\"Winter's\""));
  }
  EXPECT_EQ(vecs("a"), expr("author_name:=\"George R.R. Martin\""));
  EXPECT_TRUE(expr("author_name:=\"george r.r. martin\"").empty());

  auto req = run(R"({"fields":["id","author_name","contributor_names"],"limit":-1,
    "sort":[{"expr":"author_name"}],"ops":{
      "authors":{"field_facet":{"field":"author_name","limit":-1}},
      "contributors":{"field_facet":{"field":"contributor_names","limit":-1}}
    }})");
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(vecs("b", "a", "c"), ids(*req, true));
  EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("id", "b", "author_name", andre,
                                               "contributor_names", vecs(andre, george))));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{{andre, 1}, {george, 1}, {"Winter's", 1}}),
            buckets(*req->docList()->ops.at("authors")->facetResult()));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{{andre, 1}, {george, 2}, {"Winter's", 1}}),
            buckets(*req->docList()->ops.at("contributors")->facetResult()));
}

TEST_F(FieldVariantsQueryTest, facetSelectionUsesItsOwnValueTarget) {
  auto req = run(R"({"query":{"all":true},"fields":["id"],"limit":-1,"ops":{
    "authors":{"field_facet":{"field":"author","limit":0,"selected":["URSULA K. LE GUIN"]}}
  }})");
  EXPECT_EQ(hits(R"({"any_of":{"field":"author","values":["URSULA K. LE GUIN"]}})"), ids(*req));
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{{"ursula k. le guin", 2}}),
      buckets(*req->docList()->ops.at("authors")->facetResult()));

  auto tokens = run(R"({"query":{"all":true},"fields":["id"],"limit":-1,"ops":{
    "words":{"field_facet":{"field":"author__self","limit":0,"selected":["LE"]}}
  }})");
  EXPECT_EQ(hits(R"({"any_of":{"field":"author__self","values":["LE"]}})"), ids(*tokens));
}

TEST_F(FieldVariantsQueryTest, twoAnalyzersAndSearchBinding) {
  EXPECT_EQ((std::vector<std::string>{"a", "b"}), expr("title:cafe"));
  EXPECT_EQ((std::vector<std::string>{"b"}), expr("title__preserve:cafe"));
  EXPECT_EQ((std::vector<std::string>{"a"}), expr("title__preserve:caf\u00e9"));
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), hits(R"({"match":{"genre":"SCIENCE"}})"));
  EXPECT_EQ((std::vector<std::string>{"a"}), expr("genre:\"Science Fiction\""));
  EXPECT_EQ((std::vector<std::string>{"a"}), hits(R"({"phrase":{"field":"genre","text":"science fiction"}})"));
  EXPECT_TRUE(hits(R"({"match":{"genre__self":"science"}})").empty());
  EXPECT_EQ((std::vector<std::string>{"a"}), expr("genre:=\"Science Fiction\""));
  for (auto query : {
      R"({"prefix":{"field":"genre","prefix":"SCI"}})",
      R"({"fuzzy":{"field":"genre","term":"scienc","max_edits":1}})",
      R"({"wildcard":{"field":"genre","pattern":"SCI*"}})",
      R"({"regex":{"field":"genre","pattern":"SCI.*"}})"}) {
    EXPECT_EQ((std::vector<std::string>{"a", "c"}), hits(query)) << query;
  }
}

TEST_F(FieldVariantsQueryTest, numericMatchDoesNotRebindDuringLowering) {
  EXPECT_EQ((std::vector<std::string>{"a"}), hits(R"({"match":{"edition":42}})"));
  EXPECT_EQ((std::vector<std::string>{"a"}), expr("edition:42"));
  EXPECT_EQ((std::vector<std::string>{"a"}), expr("edition:\"42\""));
  EXPECT_TRUE(expr("edition:=42").empty());
  EXPECT_EQ((std::vector<std::string>{"a"}), expr("edition:=0042"));
  auto lexical = run(R"({"query":{"all":true},"fields":["id"],"limit":-1,"sort":[{"expr":"edition__label"}]})");
  EXPECT_EQ((std::vector<std::string>{"a", "d", "b", "c"}), ids(*lexical, true));
  auto numeric = run(R"({"query":{"all":true},"fields":["id"],"limit":-1,"sort":[{"expr":"edition__self"}]})");
  EXPECT_EQ((std::vector<std::string>{"c", "d", "b", "a"}), ids(*numeric, true));
}

TEST_F(FieldVariantsQueryTest, exactSyntaxAndTeachingErrors) {
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), expr("author:=\"URSULA K. LE GUIN\""));
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), expr("author:=(\"Ursula K. Le Guin\", 'George R.R. Martin')"));
  EXPECT_EQ((std::vector<std::string>{"a", "c", "d"}), expr("author__self:=(LE, ursula)"));
  EXPECT_EQ((std::vector<std::string>{"a", "c", "d"}), expr("any_of((LE, ursula), field=author__self)"));
  EXPECT_EQ((std::vector<std::string>{"a", "c", "d"}), expr("any_of(field=author__self, values=LE)"));
  for (auto expression : {"author__self:=\"Le Guin\"", "author__self:=(Le, 'Le Guin')",
                          "author:=(one two)", "author:=()", "author:=(one,)"}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(expression);
    req->execute(false);
    EXPECT_FALSE(req->ok()) << expression;
    EXPECT_NE(std::string::npos, req->errorMsg().find("expr parse error at byte")) << req->errorMsg();
    if (std::string_view(expression).starts_with("author__self")) {
      for (auto detail : {"single term", "match", "phrase", "whole-value string variant"}) {
        EXPECT_NE(std::string::npos, req->errorMsg().find(detail)) << req->errorMsg();
      }
    }
  }
  auto structured = run(R"({"query":{"any_of":{"field":"author__self","values":["Le Guin"]}}})");
  EXPECT_FALSE(structured->ok());
  EXPECT_NE(std::string::npos, structured->errorMsg().find("single term"));
}

TEST_F(FieldVariantsQueryTest, textExactMembershipUsesAnalyzedTerm) {
  auto expected = hits(R"({"match":{"author__self":"Guin!"}})");
  ASSERT_EQ((std::vector<std::string>{"a", "c"}), expected);
  EXPECT_EQ(expected, hits(R"({"any_of":{"field":"author__self","values":["Guin!"]}})"));
  EXPECT_EQ(expected, expr("author__self:=\"Guin!\""));
  EXPECT_EQ(expected, expr("author__self:=(\"!!!\", \"GUIN!\", \"Guin\")"));
  for (const char* value : {"", "!!!"}) {
    EXPECT_TRUE(hits(std::format(R"({{"any_of":{{"field":"author__self","values":["{}"]}}}})", value)).empty());
    EXPECT_TRUE(expr(std::format("author__self:=\"{}\"", value)).empty());
  }
  auto selected = run(R"({"query":{"all":true},"fields":["id"],"ops":{
    "words":{"field_facet":{"field":"author__self","limit":0,"selected":["Guin!"]}}
  }})");
  EXPECT_EQ(expected, ids(*selected));
  ASSERT_OK(selected);
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{{"guin", 2}}),
      buckets(*selected->docList()->ops.at("words")->facetResult()));
}

TEST_F(FieldVariantsQueryTest, normalizerAppliesToLiteralsAndPatterns) {
  for (auto query : {
      R"({"match":{"author__s":"LE CARR\u00c9"}})",
      R"({"any_of":{"field":"author","values":["LE CARR\u00c9"]}})",
      R"({"range":{"field":"author","gte":"LE CARR\u00c9","lte":"LE CARR\u00c9"}})",
      R"({"prefix":{"field":"author__s","prefix":"LE CARR\u00c9"}})",
      R"({"wildcard":{"field":"author__s","pattern":"LE CARR\u00c9*"}})",
      R"({"regex":{"field":"author__s","pattern":"LE CARR\u00c9.*"}})",
      R"({"fuzzy":{"field":"author__s","term":"LE CARR\u00c9","max_edits":0}})"}) {
    EXPECT_EQ((std::vector<std::string>{"d"}), hits(query)) << query;
  }
}

TEST_F(FieldVariantsQueryTest, exactLengthCheckedAfterNormalization) {
  SchemaBuilder b;
  auto& text = b.field("strict_text");
  text.type = api::FieldDef::FieldClass::TEXT;
  text.long_terms = api::FieldDef::LongTerms::REJECT;
  auto& whole = b.field("strict_string");
  whole.type = api::FieldDef::FieldClass::STRING;
  whole.long_terms = api::FieldDef::LongTerms::REJECT;
  b.normalizer(whole, {"nfkc_cf", "fold"});
  b.set(helper.collection());
  std::string longValue(256, 'x');
  for (auto field : {"strict_string", "strict_text"}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(std::string(field) + ":=" + longValue);
    req->execute(false);
    EXPECT_FALSE(req->ok());
    for (auto detail : {"expr parse error at byte", "256 bytes after normalization", "255", "whole-value"}) {
      EXPECT_NE(std::string::npos, req->errorMsg().find(detail)) << req->errorMsg();
    }
    for (auto query : {
        std::format(R"({{"any_of":{{"field":"{}","values":["{}"]}}}})", field, longValue),
        std::format(R"({{"range":{{"field":"{}","gte":"{}"}}}})", field, longValue)}) {
      auto exact = run(std::format(R"({{"query":{}}})", query));
      EXPECT_FALSE(exact->ok());
      EXPECT_NE(std::string::npos, exact->errorMsg().find("maximum is 255"));
    }
    auto selected = run(std::format(R"({{"query":{{"all":true}},"ops":{{"facet":{{
      "field_facet":{{"field":"{}","selected":["{}"]}}
    }}}}}})", field, longValue));
    EXPECT_FALSE(selected->ok());
    EXPECT_NE(std::string::npos, selected->errorMsg().find("maximum is 255"));
  }
  for (auto query : {std::format(R"({{"match":{{"strict_string":"{}"}}}})", longValue),
                     std::format(R"({{"range":{{"field":"strict_string","gte":"{}"}}}})", longValue)}) {
    auto req = run(std::format(R"({{"query":{}}})", query));
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("255")) << req->errorMsg();
  }
  std::string folded;
  for (int i = 0; i < 200; i++) folded += "\u00e9";
  ASSERT_TRUE(helper.index(flatdoc("id", "long", "strict_string", folded), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"long"}), expr("strict_string:=\"" + folded + "\""));
  EXPECT_TRUE(expr("strict_string:" + longValue + "*").empty());
}

TEST_F(FieldVariantsQueryTest, longIdQueriesAgreeWithIngestHash128) {
  std::string id(300, 'x');
  auto indexed = helper.index(flatdoc("id", id), UpdateMessage::COMMIT);
  ASSERT_TRUE(indexed.success);
  ASSERT_TRUE(indexed.errors.empty());
  std::vector<std::string> expected{std::string(230, 'x') + "131pru8lxm5t1cohchv0tgjtg"};
  EXPECT_EQ(expected, hits(std::format(R"({{"match":{{"id":"{}"}}}})", id)));
  EXPECT_EQ(expected, hits(std::format(R"({{"any_of":{{"field":"id","values":["{}"]}}}})", id)));
  EXPECT_EQ(expected, hits(std::format(R"({{"range":{{"field":"id","gte":"{}","lte":"{}"}}}})", id, id)));
  EXPECT_EQ(expected, expr("id:=" + id));
}

TEST_F(FieldVariantsQueryTest, hash128ExactValuesRangesFacetsAndSort) {
  SchemaBuilder b;
  auto& whole = b.field("whole");
  whole.type = api::FieldDef::FieldClass::STRING;
  whole.stored = true;
  b.normalizer(whole, {"nfkc_cf"});
  auto& token = b.field("token");
  token.type = api::FieldDef::FieldClass::TEXT;
  b.analyzer(token, "whitespace", {"nfkc_cf"});
  b.set(helper.collection());
  std::vector<std::string> values{std::string(260, 'X') + std::string(40, 'A'),
                                 std::string(260, 'X') + std::string(40, 'B'),
                                 std::string(300, 'Y')};
  std::vector<std::pair<std::string, std::string>> termsAndIds;
  for (size_t i = 0; i < values.size(); i++) {
    auto id = "long" + std::to_string(i);
    ASSERT_TRUE(helper.index(flatdoc("id", id, "whole", values[i], "token", values[i]),
                             UpdateMessage::COMMIT).success);
    std::string normalized = values[i];
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](char c) { return c + ('a' - 'A'); });
    PackedTerm::TermBuffer scratch;
    termsAndIds.emplace_back(*PackedTerm::fitTerm(TermPolicy::HASH128, normalized, scratch), id);
    for (auto field : {"whole", "token"}) {
      SCOPED_TRACE(field);
      auto expected = std::vector<std::string>{id};
      EXPECT_EQ(expected, expr(std::string(field) + ":=" + values[i]));
      for (auto query : {
          std::format(R"({{"match":{{"{}":"{}"}}}})", field, values[i]),
          std::format(R"({{"any_of":{{"field":"{}","values":["{}"]}}}})", field, values[i]),
          std::format(R"({{"range":{{"field":"{}","gte":"{}","lte":"{}"}}}})", field, values[i], values[i]),
          std::format(R"({{"fuzzy":{{"field":"{}","term":"{}","max_edits":0}}}})", field, values[i])}) {
        EXPECT_EQ(expected, hits(query)) << query;
      }
      EXPECT_TRUE(hits(std::format(R"({{"range":{{"field":"{}","gt":"{}","lte":"{}"}}}})",
                                  field, values[i], values[i])).empty());
      auto selected = run(std::format(R"({{"query":{{"all":true}},"fields":["id"],"ops":{{
        "facet":{{"field_facet":{{"field":"{}","limit":0,"selected":["{}","{}"]}}}}
      }}}})", field, values[i], values[i]));
      EXPECT_EQ(expected, ids(*selected));
      ASSERT_TRUE(selected->ok()) << selected->errorMsg();
      EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{{termsAndIds.back().first, 1}}),
                buckets(*selected->docList()->ops.at("facet")->facetResult()));
    }
  }
  std::sort(termsAndIds.begin(), termsAndIds.end());
  std::vector<std::string> expectedOrder;
  std::vector<std::pair<std::string, int64_t>> expectedBuckets;
  for (const auto& [term, id] : termsAndIds) {
    expectedOrder.push_back(id);
    expectedBuckets.emplace_back(term, 1);
  }
  EXPECT_EQ("long2", expectedOrder.back()); // Original order is retained within the literal prefix.
  auto sorted = run(R"({"query":{"exists":{"field":"whole"}},"fields":["id","whole"],
    "sort":[{"expr":"whole"}],"ops":{"facet":{"field_facet":{"field":"whole","limit":-1}}}})");
  EXPECT_EQ(expectedOrder, ids(*sorted, true));
  ASSERT_TRUE(sorted->ok()) << sorted->errorMsg();
  EXPECT_EQ(expectedBuckets, buckets(*sorted->docList()->ops.at("facet")->facetResult()));
  EXPECT_TRUE(containsDoc(sorted->getDocs(), flatdoc("id", "long0", "whole", values[0])));
}

TEST_F(FieldVariantsQueryTest, hash128LongPatternsUseKeptLiteralPrefix) {
  std::string first = std::string(260, 'x') + std::string(40, 'a');
  std::string second = std::string(260, 'x') + std::string(40, 'b');
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "first", "value_s", first, "token_w", first),
    flatdoc("id", "second", "value_s", second, "token_w", second),
    flatdoc("id", "short", "value_s", std::string(240, 'x'), "token_w", std::string(240, 'x'))
  }, UpdateMessage::COMMIT).success);
  auto expected = std::vector<std::string>{"first", "second", "short"};
  for (auto field : {"value_s", "token_w"}) {
    for (size_t size : {230u, 231u, 255u, 280u, 300u}) {
      auto prefix = first.substr(0, size);
      EXPECT_EQ(expected, expr(std::string(field) + ":" + prefix + "*"));
      EXPECT_EQ(expected, hits(std::format(R"({{"prefix":{{"field":"{}","prefix":"{}"}}}})", field, prefix)));
      EXPECT_EQ(expected, hits(std::format(R"({{"wildcard":{{"field":"{}","pattern":"{}*"}}}})", field, prefix)));
      EXPECT_EQ(expected, hits(std::format(R"({{"regex":{{"field":"{}","pattern":"{}.*"}}}})", field, prefix)));
      if (size > 230) {
        // A literal-only pattern is still a pattern, and also widens.
        EXPECT_EQ(expected, hits(std::format(R"({{"wildcard":{{"field":"{}","pattern":"{}"}}}})", field, prefix)));
        EXPECT_EQ(expected, hits(std::format(R"json({{"regex":{{"field":"{}","pattern":"{}(a|b)"}}}})json", field, prefix)));
      }
    }
  }
  std::string utf8;
  for (int i = 0; i < 150; i++) utf8 += "\xc3\xa9";
  ASSERT_TRUE(helper.index(flatdoc("id", "utf8", "value_s", utf8), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"utf8"}), expr("value_s:=" + utf8));
  EXPECT_EQ((std::vector<std::string>{"utf8"}), hits(std::format(
      R"({{"regex":{{"field":"value_s","pattern":"{}.*"}}}})", utf8.substr(0, 234))));
}

TEST_F(FieldVariantsQueryTest, scopesBindEachLeafAndSelectorsFreeze) {
  EXPECT_TRUE(expr("author:(Ursula AND <=M)").empty());
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), expr("author__self:(Ursula AND <=M)"));
  EXPECT_TRUE(expr("author__s:(Ursula AND <=M)").empty());
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), expr("author__s:(\"Ursula K. Le Guin\" AND >=M)"));
  EXPECT_EQ((std::vector<std::string>{"a"}), expr("book_number:(0042 AND >=40)"));
  EXPECT_TRUE(expr("book_number__self:(0042 AND >=40)").empty());
  EXPECT_EQ((std::vector<std::string>{"a"}), expr("book_number__s:(42 AND >=40)"));
}

TEST_F(FieldVariantsQueryTest, simpleQueryDeduplicatesAliasesAndRestrictsExactTargets) {
  auto req = run(R"({"query":{"simple_query":{"q":"Le","fields":["author","author__self"]}},"fields":["id"],"limit":-1})");
  EXPECT_EQ((std::vector<std::string>{"a", "c", "d"}), ids(*req));
  const auto& top = std::get<api::TopDocs>(req->rawRequest().ops.at("q")->kind);
  ASSERT_NE(nullptr, std::get_if<api::Match>(&top.query->kind));
  EXPECT_EQ("author", std::get<api::Match>(top.query->kind).field);

  auto allowed = run(R"({"query":{"simple_query":{"q":"author__self:Le","fields":["title"],"allowed_fields":["author"]}},"fields":["id"],"limit":-1})");
  EXPECT_EQ((std::vector<std::string>{"a", "c", "d"}), ids(*allowed));
  EXPECT_FALSE(allowed->hasWarning("field_narrowed"));
  auto denied = run(R"({"query":{"simple_query":{"q":"author__s:Le","fields":["title"],"allowed_fields":["author"]}},"fields":["id"],"limit":-1})");
  EXPECT_TRUE(ids(*denied).empty());
  EXPECT_TRUE(denied->hasWarning("field_narrowed"));
  auto searchDefault = run(R"({"query":{"simple_query":{"q":"genre__t:Science","fields":["genre","genre__t"],"allowed_fields":["genre"]}},"fields":["id"],"limit":-1})");
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), ids(*searchDefault));
  EXPECT_FALSE(searchDefault->hasWarning("field_narrowed"));
}

TEST_F(FieldVariantsQueryTest, columnCapabilitiesAndDynamicValueStats) {
  for (auto expression : {"title", "col(\"title\")", "author__self"}) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q").allQuery();
    qb::sort(top, expression);
    req->execute(false);
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("no value column")) << req->errorMsg();
    EXPECT_NE(std::string::npos, req->errorMsg().find("string variant")) << req->errorMsg();
  }
  auto req = run(R"json({"ops":{
    "mean":{"expr_op":{"expr":"avg(book_number)"}},
    "lo":{"expr_op":{"expr":"min(book_number)"}},
    "hi":{"expr_op":{"expr":"max(book_number)"}},
    "ranges":{"range_facet":{"field":"book_number","start":0,"end":50,"gap":10}}
  }})json");
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_DOUBLE_EQ(14.25, req->scalar<double>("mean"));
  EXPECT_EQ(2, req->scalar<int64_t>("lo"));
  EXPECT_EQ(42, req->scalar<int64_t>("hi"));
  auto* facet = req->responses[0]->proto.ops.at("ranges")->facetResult();
  ASSERT_NE(nullptr, facet);
  EXPECT_EQ((std::vector<int64_t>{2, 1, 0, 0, 1}), std::vector<int64_t>(facet->counts.begin(), facet->counts.end()));
}

TEST_F(FieldVariantsQueryTest, existsUsesPrimaryAndExplicitSelectors) {
  for (auto field : {"author", "author__self", "author__s", "genre", "genre__t"}) {
    EXPECT_EQ((std::vector<std::string>{"a", "b", "c", "d"}), expr(std::string(field) + ":*"));
  }
  ASSERT_TRUE(helper.index(flatdoc("id", "empty", "author", "", "genre", ""), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c", "d", "empty"}), expr("author:*"));
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c", "d", "empty"}), expr("genre:*^2"));
}

TEST_F(FieldVariantsQueryTest, exactTokensKeepPatternAndScoreBytesLiteral) {
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "caret", "author", "x^2"),
    flatdoc("id", "tilde", "author", "x~1.5"),
    flatdoc("id", "star", "author", "x*"),
  }, UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"caret"}), expr("author:=x^2"));
  EXPECT_EQ((std::vector<std::string>{"tilde"}), expr("author:=x~1.5"));
  EXPECT_EQ((std::vector<std::string>{"star"}), expr("author:=x*"));
  EXPECT_EQ((std::vector<std::string>{"caret"}), expr("author:=\"x^2\"^3"));
  EXPECT_EQ((std::vector<std::string>{"caret", "star"}), expr("author:=(x^2, x*)^3"));
}

TEST_F(FieldVariantsQueryTest, queryLengthRejectsNormalizerExpansion) {
  SchemaBuilder b;
  auto& value = b.field("normalized");
  value.type = api::FieldDef::FieldClass::STRING;
  value.long_terms = api::FieldDef::LongTerms::REJECT;
  b.normalizer(value, {"nfkc_cf"});
  b.set(helper.collection());
  // U+0130 expands to i plus combining dot under nfkc_cf: 170 -> 255 bytes.
  std::string legal;
  for (int i = 0; i < 85; i++) legal += "\u0130";
  ASSERT_TRUE(helper.index(flatdoc("id", "cap", "normalized", legal), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"cap"}), expr("normalized:=\"" + legal + "\""));
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").exprQuery("normalized:=\"" + legal + "\u0130\"");
  req->execute(false);
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("258 bytes after normalization")) << req->errorMsg();
}

TEST_F(FieldVariantsQueryTest, longTermsTruncateAfterNormalizationAcrossValueOperations) {
  SchemaBuilder b;
  auto& whole = b.field("whole");
  whole.type = api::FieldDef::FieldClass::STRING;
  whole.long_terms = api::FieldDef::LongTerms::TRUNCATE;
  b.normalizer(whole, {"nfkc_cf"});
  auto& text = b.field("token");
  text.type = api::FieldDef::FieldClass::TEXT;
  text.long_terms = api::FieldDef::LongTerms::TRUNCATE;
  b.analyzer(text, "whitespace", {"nfkc_cf"});
  b.set(helper.collection());
  // Normalization expands a 255-byte input to 256 bytes. Truncation must
  // then back off the split combining mark, leaving 254 complete UTF-8 bytes.
  std::string input = std::string(253, 'X') + "\xc4\xb0";
  std::string prefix = std::string(253, 'x') + "i";
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "first", "whole", input, "token", "before " + input + " after"),
    flatdoc("id", "second", "whole", input + "tail", "token", input + "tail")
  }, UpdateMessage::COMMIT).success);
  for (auto field : {"whole", "token"}) {
    SCOPED_TRACE(field);
    for (auto value : {input, input + "anything", prefix}) {
      auto expected = std::vector<std::string>{"first", "second"};
      EXPECT_EQ(expected, expr(std::string(field) + ":=\"" + value + "\""));
      EXPECT_EQ(expected, hits(std::format(R"({{"match":{{"{}":"{}"}}}})", field, value)));
      EXPECT_EQ(expected, hits(std::format(R"({{"any_of":{{"field":"{}","values":["{}","{}"]}}}})", field, value, prefix)));
      EXPECT_EQ(expected, hits(std::format(R"({{"range":{{"field":"{}","gte":"{}","lte":"{}"}}}})", field, value, value)));
      EXPECT_TRUE(hits(std::format(R"({{"range":{{"field":"{}","gt":"{}","lte":"{}"}}}})", field, value, value)).empty());
      auto req = run(std::format(R"({{"query":{{"all":true}},"fields":["id"],"limit":-1,"ops":{{
        "selected":{{"field_facet":{{"field":"{}","limit":0,"selected":["{}","{}"]}}}}
      }}}})", field, value, prefix));
      EXPECT_EQ(expected, ids(*req));
      ASSERT_TRUE(req->ok()) << req->errorMsg();
      EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{{prefix, 2}}),
                buckets(*req->docList()->ops.at("selected")->facetResult()));
    }
  }
}

TEST_F(FieldVariantsQueryTest, exactVariablesAndExplainDoNotMutateInput) {
  for (auto expression : {"any_of($values, field=author)", "any_of(values=$values, field=author)",
                          "author:=$values", "author:=($one, 'George R.R. Martin')"}) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q").exprQuery(expression).fields({"id"}).limit(-1);
    auto& query = std::get<api::ExprQuery>(top.rawQuery().kind);
    auto* vars = api::build::allocArray(query.vars, 2, req->mr);
    auto* values = api::build::allocMessage<api::Val>(req->mr);
    auto& strings = values->kind.emplace<api::ArrStr>();
    auto* data = api::build::allocArray(strings.v, 2, req->mr);
    data[0] = "Ursula K. Le Guin";
    data[1] = "George R.R. Martin";
    vars[0] = {"values", values};
    vars[1] = {"one", qb::valStr(req->mr, "Ursula K. Le Guin")};
    auto notes = helper.getSearchEngine().explain(req->rawRequest());
    EXPECT_TRUE(std::ranges::contains(notes, std::string("q: author -> author__s")));
    ASSERT_TRUE(std::holds_alternative<api::ExprQuery>(top.rawQuery().kind));
    EXPECT_EQ(expression, std::get<api::ExprQuery>(top.rawQuery().kind).q);
    req->execute(false);
    EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), ids(*req));
  }
}

TEST_F(FieldVariantsQueryTest, knnGeoAndExistsUsePrimaryUnlessExplicit) {
  SchemaBuilder b;
  auto& vec = b.field("vec");
  vec.type = api::FieldDef::FieldClass::VECTOR;
  vec.dims = 2;
  vec.metric = api::VectorMetric::L2;
  auto& cosine = b.variant(vec, "cosine");
  cosine.type = api::FieldDef::FieldClass::VECTOR;
  cosine.dims = 2;
  cosine.metric = api::VectorMetric::COSINE;
  vec.defaults.emplace().search = "cosine";
  auto& point = b.field("point");
  point.type = api::FieldDef::FieldClass::GEO_POINT;
  b.variant(point, "copy").type = api::FieldDef::FieldClass::GEO_POINT;
  point.defaults.emplace().search = "copy";
  b.set(helper.collection());
  auto indexed = helper.indexAll(std::array{
    flatdoc("id", "v1", "vec", std::vector<float>{1, 1}, "point", std::vector<double>{20, 10}),
    flatdoc("id", "v2", "vec", std::vector<float>{100, 0}, "point", std::vector<double>{40, 30}),
    flatdoc("id", "zero", "vec", std::vector<float>{0, 0}),
  }, UpdateMessage::COMMIT);
  ASSERT_TRUE(indexed.success);
  ASSERT_TRUE(indexed.errors.empty());
  for (auto field : {"vec", "vec__self", "vec__cosine"}) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q").fields({"id"}).limit(1);
    top.rawQuery() = qb::knn(top.mr(), field, {1, 1}, 1, 0, true);
    req->execute(false);
    EXPECT_EQ((std::vector<std::string>{"v1"}), ids(*req));
  }
  // L2 and cosine give different results for this query. Defaults.search must
  // not select cosine for a bare knn request.
  for (auto field : {"vec", "vec__cosine"}) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q").fields({"id"}).limit(1);
    top.rawQuery() = qb::knn(top.mr(), field, {2, 0}, 1, 0, true);
    req->execute(false);
    EXPECT_EQ((std::vector<std::string>{std::string_view(field) == "vec" ? "v1" : "v2"}), ids(*req));
  }
  EXPECT_EQ((std::vector<std::string>{"v1", "v2", "zero"}), expr("vec:*"));
  EXPECT_EQ((std::vector<std::string>{"v1", "v2"}), expr("vec__cosine:*"));
  for (auto field : {"point", "point__self", "point__copy"}) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q").fields({"id"});
    top.rawQuery() = qb::geoBox(top.mr(), field, 9, 11, 19, 21);
    req->execute(false);
    EXPECT_EQ((std::vector<std::string>{"v1"}), ids(*req));
  }
  EXPECT_EQ((std::vector<std::string>{"v1"}), hits(R"({"geo_distance":{"field":"point__self","lat":10,"lon":20,"radius_meters":100}})"));
}
