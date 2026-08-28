#include <gtest/gtest.h>

#include <algorithm>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/build.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

namespace api = luxir::api;

api::Val strVal(std::string_view value) {
  api::Val val;
  val.kind = value;
  return val;
}

api::Val intVal(int64_t value) {
  api::Val val;
  val.kind = value;
  return val;
}

api::Query inQuery(std::pmr::memory_resource& mr, std::string_view field,
                   std::initializer_list<api::Val> values) {
  api::Query query;
  auto& in = query.kind.emplace<api::InQuery>();
  in.field = api::build::arenaStr(mr, field);
  api::Val* stored = api::build::allocArray(in.values, values.size(), mr);
  std::copy(values.begin(), values.end(), stored);
  return query;
}

std::vector<std::string> resultIds(const LocalReq& req) {
  std::vector<std::string> ids;
  for (const Doc& doc : req.getDocs()) {
    const FieldVal* id = find(doc, "id");
    if (id != nullptr) ids.push_back(std::get<std::string>(*id));
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

} // namespace

class InQueryTest : public LuxirTest {
public:
  CollectionHelper helper;

  void SetUp() override {
    std::vector<Doc> docs;
    docs.push_back(flatdoc(
        "id", "a", "brand_s", "acme", "body_un", "red MixedCase",
        "nums_is", vec_i(1, 3), "when_dt", "2024-01-01",
        "col_sc", "raw-a"));
    docs.push_back(flatdoc(
        "id", "b", "brand_s", "globex", "body_un", "red lower",
        "nums_is", vec_i(2), "when_dt", "2024-01-15",
        "col_sc", "raw-b"));
    docs.push_back(flatdoc(
        "id", "c", "brand_s", "initech", "body_un", "blue mixedcase",
        "nums_is", vec_i(3, 4), "when_dt", "2024-02-01",
        "col_sc", "raw-c"));
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
};

TEST_F(InQueryTest, indexedTermsSingleParityAndUnion) {
  auto single = localReq(helper.getSearchEngine());
  auto& singleTop = single->collection("main").topDocs("q");
  singleTop.rawQuery() = inQuery(singleTop.mr(), "brand_s", {strVal("acme")});
  singleTop.fields({"id"}).limit(-1);
  single->execute();
  ASSERT_TRUE(single->ok()) << single->errorMsg();

  auto match = localReq(helper.getSearchEngine());
  match->collection("main").topDocs("q").matchQuery("brand_s", "acme")
      .fields({"id"}).limit(-1);
  match->execute();
  ASSERT_TRUE(match->ok()) << match->errorMsg();
  EXPECT_EQ(resultIds(*match), resultIds(*single));

  auto multi = localReq(helper.getSearchEngine());
  auto& multiTop = multi->collection("main").topDocs("q");
  multiTop.rawQuery() = inQuery(
      multiTop.mr(), "brand_s", {strVal("globex"), strVal("missing"),
                                  strVal("acme")});
  multiTop.fields({"id"}).limit(-1);
  multi->execute();
  ASSERT_TRUE(multi->ok()) << multi->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "b"}), resultIds(*multi));
}

TEST_F(InQueryTest, textValuesAreRawIndexedTerms) {
  auto raw = localReq(helper.getSearchEngine());
  auto& rawTop = raw->collection("main").topDocs("q");
  rawTop.rawQuery() = inQuery(rawTop.mr(), "body_un", {strVal("MixedCase")});
  rawTop.fields({"id"}).limit(-1);
  raw->execute();
  ASSERT_TRUE(raw->ok()) << raw->errorMsg();
  EXPECT_TRUE(resultIds(*raw).empty());

  auto indexed = localReq(helper.getSearchEngine());
  auto& indexedTop = indexed->collection("main").topDocs("q");
  indexedTop.rawQuery() = inQuery(
      indexedTop.mr(), "body_un", {strVal("mixedcase")});
  indexedTop.fields({"id"}).limit(-1);
  indexed->execute();
  ASSERT_TRUE(indexed->ok()) << indexed->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), resultIds(*indexed));

  auto analyzed = localReq(helper.getSearchEngine());
  analyzed->collection("main").topDocs("q").matchQuery(
      "body_un", "MixedCase").fields({"id"}).limit(-1);
  analyzed->execute();
  ASSERT_TRUE(analyzed->ok()) << analyzed->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), resultIds(*analyzed));
}

TEST_F(InQueryTest, numericMultiValueMatchesOnceAndScoresConstant) {
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->collection("main").topDocs("q");
  top.rawQuery() = inQuery(top.mr(), "nums_is", {intVal(3), intVal(1)});
  top.withStats().fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), resultIds(*req));

  const auto& scores = std::get<api::ColFloat>(
      req->docList()->columns.at("_score_").kind).v;
  ASSERT_EQ(2u, scores.size());
  EXPECT_FLOAT_EQ(scores[0], scores[1]);
}

TEST_F(InQueryTest, dateIsOneIngestInstantNotAPartialRange) {
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->collection("main").topDocs("q");
  top.rawQuery() = inQuery(top.mr(), "when_dt", {strVal("2024-01")});
  top.fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a"}), resultIds(*req));
}

TEST_F(InQueryTest, rawStringColumnUsesExactMembership) {
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->collection("main").topDocs("q");
  top.rawQuery() = inQuery(
      top.mr(), "col_sc", {strVal("raw-c"), strVal("raw-a")});
  top.fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), resultIds(*req));
}

TEST_F(InQueryTest, topDocsFilterAndBooleanClauseCompose) {
  auto filterReq = localReq(helper.getSearchEngine());
  auto& filterTop = filterReq->collection("main").topDocs("q");
  api::Query filter = inQuery(
      filterTop.mr(), "brand_s", {strVal("acme"), strVal("globex")});
  filterTop.allQuery().filter(filter).fields({"id"}).limit(-1);
  filterReq->execute();
  ASSERT_TRUE(filterReq->ok()) << filterReq->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "b"}), resultIds(*filterReq));

  auto booleanReq = localReq(helper.getSearchEngine());
  auto& booleanTop = booleanReq->collection("main").topDocs("q");
  api::Query selected = inQuery(
      booleanTop.mr(), "brand_s", {strVal("acme"), strVal("initech")});
  api::Query red = qb::match(booleanTop.mr(), "body_un", "red");
  booleanTop.rawQuery() = qb::boolean(booleanTop.mr(), {red, selected});
  booleanTop.fields({"id"}).limit(-1);
  booleanReq->execute();
  ASSERT_TRUE(booleanReq->ok()) << booleanReq->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a"}), resultIds(*booleanReq));
}

TEST_F(InQueryTest, validationErrorsAreParseTime) {
  auto expectError = [&](auto&& build, std::string_view detail) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q");
    top.rawQuery() = build(top.mr());
    req->execute();
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find(detail))
        << req->errorMsg();
  };

  expectError([](std::pmr::memory_resource& mr) {
    return inQuery(mr, "brand_s", {});
  }, "requires at least one value");
  expectError([](std::pmr::memory_resource& mr) {
    return inQuery(mr, "nums_is", {intVal(1), strVal("1")});
  }, "duplicate values after coercion");
  expectError([](std::pmr::memory_resource& mr) {
    return inQuery(mr, "emb_v", {intVal(1)});
  }, "emb_v");
}
