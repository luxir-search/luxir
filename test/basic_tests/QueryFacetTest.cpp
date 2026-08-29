#include <gtest/gtest.h>

#include <array>
#include <map>
#include <memory>
#include <memory_resource>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace {

namespace api = luxir::api;
namespace build = luxir::api::build;
using namespace luxir;
using namespace luxir::test;

api::QueryFacet& queryFacet(OpCursor& cursor) {
  return cursor.rawOp().kind.emplace<api::QueryFacet>();
}

api::QueryBucket* addBuckets(api::QueryFacet& facet,
                             std::pmr::memory_resource& mr,
                             size_t count) {
  return build::allocArray(facet.buckets, count, mr);
}

void setBucket(api::QueryBucket& bucket, std::pmr::memory_resource& mr,
               std::string_view name, const api::Query& query) {
  bucket.name = build::arenaStr(mr, name);
  auto* stored = build::allocMessage<api::Query>(mr);
  *stored = query;
  bucket.query = stored;
}

void setSelected(api::QueryFacet& facet, std::pmr::memory_resource& mr,
                 std::initializer_list<std::string_view> names,
                 bool scalar = false) {
  auto* selected = build::allocMessage<api::Val>(mr);
  if (scalar) {
    selected->kind = build::arenaStr(mr, *names.begin());
  } else {
    auto& strings = selected->kind.emplace<api::ArrStr>();
    auto* values = build::allocArray(strings.v, names.size(), mr);
    size_t i = 0;
    for (std::string_view name : names) {
      values[i++] = build::arenaStr(mr, name);
    }
  }
  facet.selected = selected;
}

void expectError(LocalReq& request, std::string_view text) {
  ExpectLog quiet("Search request failed:");
  request.execute();
  ASSERT_FALSE(request.ok());
  EXPECT_NE(std::string::npos, request.errorMsg().find(text))
      << request.errorMsg();
}

std::vector<std::pair<std::string, int64_t>> queryRows(
    const api::FacetResult& result) {
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  std::vector<std::pair<std::string, int64_t>> rows;
  for (size_t i = 0; i < ids.size(); i++) {
    rows.emplace_back(ids[i], result.counts[i]);
  }
  return rows;
}

api::ExprOp& addExprOp(api::QueryFacet& facet,
                       std::pmr::memory_resource& mr,
                       std::string_view name,
                       std::string_view expression,
                       size_t capacity = 1) {
  auto* op = build::mapSlot<api::SearchOp>(facet.ops, capacity, name, mr);
  auto& expr = op->kind.emplace<api::ExprOp>();
  expr.expr = build::arenaStr(mr, expression);
  return expr;
}

api::FieldFacet& addFieldFacetOp(api::QueryFacet& facet,
                                 std::pmr::memory_resource& mr,
                                 std::string_view name,
                                 std::string_view field,
                                 size_t capacity = 1) {
  auto* op = build::mapSlot<api::SearchOp>(
      facet.ops, capacity, name, mr);
  auto& child = op->kind.emplace<api::FieldFacet>();
  child.field = build::arenaStr(mr, field);
  child.limit = -1;
  return child;
}

TEST(QueryFacetJson, ObjectFormPreservesOrderAndQuerySugar) {
  std::pmr::monotonic_buffer_resource mr;
  api::SearchOp op;
  ASSERT_TRUE(api::read_json(op, R"json({"query_facet":{
    "buckets":{
      "cheap":"price_i:[* TO 100]",
      "mid":{"range":{"field":"price_i","gte":100,"lt":1000}}
    },
    "ops":{"metric":{"expr_op":"count"}},
    "selected":"cheap"
  }})json", mr));

  const auto& facet = std::get<api::QueryFacet>(op.kind);
  ASSERT_EQ(2u, facet.buckets.size());
  EXPECT_EQ("cheap", facet.buckets[0].name);
  EXPECT_EQ("mid", facet.buckets[1].name);
  ASSERT_TRUE(facet.buckets[0].query.has_value());
  EXPECT_EQ("price_i:[* TO 100]",
            std::get<api::ExprQuery>(facet.buckets[0].query->kind).q);
  EXPECT_TRUE(std::holds_alternative<api::RangeQuery>(
      facet.buckets[1].query->kind));
  ASSERT_TRUE(facet.selected.has_value());
  EXPECT_EQ("cheap", std::get<std::string_view>(facet.selected->kind));
  ASSERT_NE(nullptr, facet.ops.find("metric"));
}

TEST(QueryFacetJson, RejectsInvalidBucketObjectsAndUnknownKeys) {
  for (std::string_view json : {
           R"({"query_facet":{"buckets":{}}})",
           R"({"query_facet":{"buckets":{"":{"all":true}}}})",
           R"({"query_facet":{"buckets":{"a":{"all":true},"a":{"all":true}}}})",
           R"({"query_facet":{"buckets":[{"name":"a","query":{"all":true}}]}})",
           R"({"query_facet":{"buckets":{"a":{"all":true}},"bogus":1}})",
       }) {
    std::pmr::monotonic_buffer_resource mr;
    api::SearchOp op;
    std::string error;
    EXPECT_FALSE(api::read_json(op, json, mr, &error)) << json;
    EXPECT_FALSE(error.empty()) << json;
  }
}

TEST(QueryFacetJson, SelectedArrayAndEchoUseObjectForm) {
  std::pmr::monotonic_buffer_resource mr;
  api::SearchOp op;
  ASSERT_TRUE(api::read_json(op, R"json({"query_facet":{
    "buckets":{"cheap":"price_i:[* TO 100]","mid":{"all":true}},
    "ops":{"metric":{"expr_op":"count"}},
    "selected":["cheap","mid"],"selection_mode":"all"
  }})json", mr));
  const auto& facet = std::get<api::QueryFacet>(op.kind);
  ASSERT_TRUE(facet.selected.has_value());
  ASSERT_TRUE(std::holds_alternative<api::ArrStr>(facet.selected->kind));
  EXPECT_EQ(api::SelectionMode::ALL, facet.selection_mode);

  std::string encoded;
  ASSERT_TRUE(api::write_json(op, encoded));
  EXPECT_EQ(R"({"query_facet":{"buckets":{"cheap":{"expr":{"q":"price_i:[* TO 100]"}},"mid":{"all":true}},"ops":{"metric":{"expr_op":{"expr":"count"}}},"selected":["cheap","mid"],"selection_mode":"all"}})",
            encoded);

  std::pmr::monotonic_buffer_resource echoMr;
  api::SearchOp echoed;
  ASSERT_TRUE(api::read_json(echoed, encoded, echoMr));
  const auto& echoedFacet = std::get<api::QueryFacet>(echoed.kind);
  ASSERT_EQ(2u, echoedFacet.buckets.size());
  EXPECT_EQ("cheap", echoedFacet.buckets[0].name);
  EXPECT_EQ("mid", echoedFacet.buckets[1].name);
}

class QueryFacetParserTest : public LuxirTest {
protected:
  std::unique_ptr<CollectionHelper> helper;

  void SetUp() override {
    helper = std::make_unique<CollectionHelper>();
  }

  api::QueryFacet& addValidFacet(OpCursor& cursor) {
    auto& facet = queryFacet(cursor);
    auto* buckets = addBuckets(facet, cursor.mr(), 2);
    setBucket(buckets[0], cursor.mr(), "cheap", qb::all());
    setBucket(buckets[1], cursor.mr(), "mid",
              qb::match(cursor.mr(), "brand_s", "acme"));
    return facet;
  }
};

TEST_F(QueryFacetParserTest, ProgrammaticNamesAndSelectionsAreValidated) {
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    queryFacet(cursor);
    expectError(*request, "query_facet buckets must be nonempty");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = queryFacet(cursor);
    auto* buckets = addBuckets(facet, cursor.mr(), 1);
    setBucket(buckets[0], cursor.mr(), "", qb::all());
    expectError(*request, "query_facet bucket names must be nonempty");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = queryFacet(cursor);
    auto* buckets = addBuckets(facet, cursor.mr(), 2);
    setBucket(buckets[0], cursor.mr(), "same", qb::all());
    setBucket(buckets[1], cursor.mr(), "same", qb::all());
    expectError(*request, "duplicate query_facet bucket name 'same'");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    setSelected(facet, cursor.mr(), {"unknown"}, true);
    expectError(*request, "selected bucket name 'unknown'");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    setSelected(facet, cursor.mr(), {"cheap", "cheap"});
    expectError(*request, "duplicate selected bucket name 'cheap'");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    auto* selected = build::allocMessage<api::Val>(cursor.mr());
    selected->kind = int64_t{1};
    facet.selected = selected;
    expectError(*request, "selected values must be strings");
  }
}

TEST_F(QueryFacetParserTest, SelectedPlacementMatchesOtherFacets) {
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    setSelected(facet, cursor.mr(), {"cheap"}, true);
    expectError(*request, "facet 'prices' at request root: selected is only supported on facets directly inside TopDocs.ops");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& outer = request->facet("outer", "brand_s");
    auto& cursor = outer.facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    setSelected(facet, cursor.mr(), {"cheap"}, true);
    expectError(*request, "facet 'prices' at nested facet bucket: selected is only supported on facets directly inside TopDocs.ops");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& fusion = request->topDocs("f").rawOp().kind.emplace<api::Fusion>();
    auto* child = build::mapSlot<api::SearchOp>(
        fusion.ops, 1, "prices", request->mr);
    auto& facet = child->kind.emplace<api::QueryFacet>();
    auto* buckets = addBuckets(facet, request->mr, 1);
    setBucket(buckets[0], request->mr, "cheap", qb::all());
    setSelected(facet, request->mr, {"cheap"}, true);
    expectError(*request, "facet 'prices' at Fusion.ops: selected is only supported on facets directly inside TopDocs.ops");
  }
}

TEST_F(QueryFacetParserTest, SelectionModeRequiresSelected) {
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    facet.selection_mode = api::SelectionMode::ALL;
    expectError(*request,
                "facet 'prices': selection_mode requires nonempty selected");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    facet.selection_mode = (api::SelectionMode)99;
    expectError(*request, "facet 'prices': selection_mode must be ANY or ALL");
  }
}

TEST_F(QueryFacetParserTest, ValidationPrecedesExecution) {
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = queryFacet(cursor);
    auto* buckets = addBuckets(facet, cursor.mr(), 1);
    buckets[0].name = build::arenaStr(cursor.mr(), "cheap");
    expectError(*request, "query_facet bucket 'cheap' requires a query");
  }
  {
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = queryFacet(cursor);
    auto* buckets = addBuckets(facet, cursor.mr(), 1);
    setBucket(buckets[0], cursor.mr(), "bad",
              qb::match(cursor.mr(), "nope_x", "acme"));
    expectError(*request, "nope_x");
  }
  {
    helper->index(flatdoc("id", "1", "brand_s", "acme"),
                  UpdateMessage::COMMIT);
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().getNumber()
        .facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    setSelected(facet, cursor.mr(), {"cheap", "mid"});
    facet.selection_mode = api::SelectionMode::ALL;
    request->execute(false);
    ASSERT_TRUE(request->ok()) << request->errorMsg();
    EXPECT_EQ(1, request->getMatchCount("q"));
    EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                  {"cheap", 1}, {"mid", 1}}),
              queryRows(*request->docList("q")->ops.at("prices")->facetResult()));
  }
}

TEST_F(QueryFacetParserTest, OrderedOverlappingBucketsRespectIncomingDomain) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "scope_s", "keep", "brand_s", "acme", "price_i", 50),
    flatdoc("id", "2", "scope_s", "keep", "brand_s", "acme", "price_i", 100),
    flatdoc("id", "3", "scope_s", "keep", "brand_s", "beta", "price_i", 150),
    flatdoc("id", "4", "scope_s", "drop", "brand_s", "beta", "price_i", 250),
    flatdoc("id", "5", "scope_s", "keep", "brand_s", "gamma", "price_i", 900),
  }, UpdateMessage::COMMIT);

  auto request = localReq(helper.getSearchEngine());
  auto& top = request->topDocs("q").allQuery().getNumber()
      .matchFilter("scope_s", "keep");
  auto& cursor = top.facet("tiers", "brand_s");
  auto& facet = queryFacet(cursor);
  auto* buckets = addBuckets(facet, cursor.mr(), 4);
  setBucket(buckets[0], cursor.mr(), "cheap",
            qb::range(cursor.mr(), "price_i", nullptr, nullptr, nullptr,
                      qb::valI64(cursor.mr(), 100)));
  setBucket(buckets[1], cursor.mr(), "mid",
            qb::range(cursor.mr(), "price_i",
                      qb::valI64(cursor.mr(), 100), nullptr, nullptr,
                      qb::valI64(cursor.mr(), 300)));
  setBucket(buckets[2], cursor.mr(), "under_200",
            qb::range(cursor.mr(), "price_i", nullptr, nullptr, nullptr,
                      qb::valI64(cursor.mr(), 200)));
  setBucket(buckets[3], cursor.mr(), "zero",
            qb::match(cursor.mr(), "brand_s", "none"));
  request->execute(false);

  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(4, request->getMatchCount("q"));
  const auto& result = *request->docList("q")->ops.at("tiers")->facetResult();
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"cheap", 1}, {"mid", 2}, {"under_200", 3}, {"zero", 0}}),
            queryRows(result));
  EXPECT_FALSE(result.total_buckets.has_value());
  EXPECT_FALSE(result.missing.has_value());
  EXPECT_EQ(0, result.offset);
}

TEST_F(QueryFacetParserTest, JsonExprBucketsExecuteEndToEnd) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "brand_s", "acme", "price_i", 50),
    flatdoc("id", "2", "brand_s", "beta", "price_i", 100),
    flatdoc("id", "3", "brand_s", "acme", "price_i", 150),
  }, UpdateMessage::COMMIT);

  auto request = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"ops":{"q":{"top_docs":{
    "query":{"all":true},"ops":{"tiers":{"query_facet":{"buckets":{
      "cheap":"price_i:[* TO 100]","acme":"brand_s:acme"
    }}}}
  }}}})json", request->rawRequest(), request->mr);
  request->execute(false);

  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"cheap", 2}, {"acme", 2}}),
            queryRows(*request->docList("q")->ops.at("tiers")->facetResult()));
}

TEST_F(QueryFacetParserTest, PerBucketExprAndStringFacetSubOps) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "brand_s", "acme", "price_i", 20),
    flatdoc("id", "2", "brand_s", "beta", "price_i", 80),
  }, UpdateMessage::COMMIT);
  helper.indexAll(std::array{
    flatdoc("id", "3", "brand_s", "acme", "price_i", 120),
    flatdoc("id", "4", "brand_s", "gamma", "price_i", 180),
  }, UpdateMessage::COMMIT);

  auto request = localReq(helper.getSearchEngine());
  auto& cursor = request->topDocs("q").allQuery().facet("tiers", "brand_s");
  auto& facet = queryFacet(cursor);
  auto* buckets = addBuckets(facet, cursor.mr(), 2);
  setBucket(buckets[0], cursor.mr(), "cheap",
            qb::range(cursor.mr(), "price_i", nullptr, nullptr, nullptr,
                      qb::valI64(cursor.mr(), 100)));
  setBucket(buckets[1], cursor.mr(), "all", qb::all());
  addExprOp(facet, cursor.mr(), "avg_price", "avg(price_i)", 2);
  addFieldFacetOp(facet, cursor.mr(), "brands", "brand_s", 2);
  request->execute();

  ASSERT_TRUE(request->ok()) << request->errorMsg();
  const auto& result = *request->docList("q")->ops.at("tiers")->facetResult();
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"cheap", 2}, {"all", 4}}), queryRows(result));
  const auto& avgs = std::get<api::ArrVal>(
      result.ops.at("avg_price")->kind).v;
  ASSERT_EQ(2u, avgs.size());
  EXPECT_DOUBLE_EQ(50.0, avgs[0].asDouble());
  EXPECT_DOUBLE_EQ(100.0, avgs[1].asDouble());

  const auto& brands = std::get<api::ArrVal>(result.ops.at("brands")->kind).v;
  ASSERT_EQ(2u, brands.size());
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"acme", 1}, {"beta", 1}}),
            queryRows(std::get<api::FacetResult>(brands[0].kind)));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"acme", 2}, {"beta", 1}, {"gamma", 1}}),
            queryRows(std::get<api::FacetResult>(brands[1].kind)));
}

TEST_F(QueryFacetParserTest, QueryFacetNestsUnderFieldFacetBuckets) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "brand_s", "acme", "price_i", 20),
    flatdoc("id", "2", "brand_s", "beta", "price_i", 80),
    flatdoc("id", "3", "brand_s", "acme", "price_i", 120),
    flatdoc("id", "4", "brand_s", "gamma", "price_i", 180),
  }, UpdateMessage::COMMIT);

  auto request = localReq(helper.getSearchEngine());
  auto& brands = request->topDocs("q").allQuery()
      .facet("brands", "brand_s").limit(-1);
  auto& nestedCursor = brands.facet("tiers", "brand_s");
  auto& tiers = queryFacet(nestedCursor);
  auto* buckets = addBuckets(tiers, nestedCursor.mr(), 2);
  setBucket(buckets[0], nestedCursor.mr(), "low",
            qb::range(nestedCursor.mr(), "price_i", nullptr, nullptr,
                      nullptr, qb::valI64(nestedCursor.mr(), 100)));
  setBucket(buckets[1], nestedCursor.mr(), "high",
            qb::range(nestedCursor.mr(), "price_i",
                      qb::valI64(nestedCursor.mr(), 100), nullptr,
                      nullptr, nullptr));
  request->execute();

  ASSERT_TRUE(request->ok()) << request->errorMsg();
  const auto& outer = *request->docList("q")->ops.at("brands")->facetResult();
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"acme", 2}, {"beta", 1}, {"gamma", 1}}), queryRows(outer));
  const auto& nested = std::get<api::ArrVal>(outer.ops.at("tiers")->kind).v;
  ASSERT_EQ(3u, nested.size());
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"low", 1}, {"high", 1}}),
            queryRows(std::get<api::FacetResult>(nested[0].kind)));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"low", 1}, {"high", 0}}),
            queryRows(std::get<api::FacetResult>(nested[1].kind)));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"low", 0}, {"high", 1}}),
            queryRows(std::get<api::FacetResult>(nested[2].kind)));
}

TEST_F(QueryFacetParserTest, SelectedAnyIsSidewaysAndFiltersSiblings) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "brand_s", "acme", "price_i", 50),
    flatdoc("id", "2", "brand_s", "acme", "price_i", 150),
    flatdoc("id", "3", "brand_s", "globex", "price_i", 50),
    flatdoc("id", "4", "brand_s", "globex", "price_i", 250),
    flatdoc("id", "5", "brand_s", "initech", "price_i", 50),
    flatdoc("id", "6", "brand_s", "initech", "price_i", 150),
  }, UpdateMessage::COMMIT);

  auto request = localReq(helper.getSearchEngine());
  auto& top = request->topDocs("q").allQuery().getNumber();
  auto& cursor = top.facet("tiers", "brand_s");
  auto& facet = queryFacet(cursor);
  auto* buckets = addBuckets(facet, cursor.mr(), 3);
  setBucket(buckets[0], cursor.mr(), "cheap",
            qb::range(cursor.mr(), "price_i", nullptr, nullptr, nullptr,
                      qb::valI64(cursor.mr(), 100)));
  setBucket(buckets[1], cursor.mr(), "mid",
            qb::range(cursor.mr(), "price_i",
                      qb::valI64(cursor.mr(), 100), nullptr, nullptr,
                      qb::valI64(cursor.mr(), 200)));
  setBucket(buckets[2], cursor.mr(), "other",
            qb::range(cursor.mr(), "price_i",
                      qb::valI64(cursor.mr(), 200), nullptr, nullptr,
                      nullptr));
  setSelected(facet, cursor.mr(), {"cheap", "mid"});
  top.facet("brands", "brand_s").limit(-1);
  request->execute();

  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(5, request->getMatchCount("q"));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"cheap", 3}, {"mid", 2}, {"other", 1}}),
            queryRows(*request->docList("q")->ops.at("tiers")->facetResult()));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"acme", 2}, {"initech", 2}, {"globex", 1}}),
            queryRows(*request->docList("q")->ops.at("brands")->facetResult()));
}

TEST_F(QueryFacetParserTest, SelectedAllUsesStrictIncomingDomain) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "brand_s", "acme", "price_i", 50),
    flatdoc("id", "2", "brand_s", "acme", "price_i", 150),
    flatdoc("id", "3", "brand_s", "globex", "price_i", 50),
    flatdoc("id", "4", "brand_s", "globex", "price_i", 250),
    flatdoc("id", "5", "brand_s", "initech", "price_i", 50),
    flatdoc("id", "6", "brand_s", "initech", "price_i", 150),
  }, UpdateMessage::COMMIT);

  auto request = localReq(helper.getSearchEngine());
  auto& top = request->topDocs("q").allQuery().getNumber();
  auto& cursor = top.facet("tiers", "brand_s");
  auto& facet = queryFacet(cursor);
  auto* buckets = addBuckets(facet, cursor.mr(), 3);
  setBucket(buckets[0], cursor.mr(), "under_200",
            qb::range(cursor.mr(), "price_i", nullptr, nullptr, nullptr,
                      qb::valI64(cursor.mr(), 200)));
  setBucket(buckets[1], cursor.mr(), "acme",
            qb::match(cursor.mr(), "brand_s", "acme"));
  setBucket(buckets[2], cursor.mr(), "all", qb::all());
  setSelected(facet, cursor.mr(), {"under_200", "acme"});
  facet.selection_mode = api::SelectionMode::ALL;
  top.facet("brands", "brand_s").limit(-1);
  request->execute();

  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(2, request->getMatchCount("q"));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"under_200", 2}, {"acme", 2}, {"all", 2}}),
            queryRows(*request->docList("q")->ops.at("tiers")->facetResult()));
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{{"acme", 2}}),
            queryRows(*request->docList("q")->ops.at("brands")->facetResult()));
}

TEST_F(QueryFacetParserTest, EmptyIndexKeepsBucketAndNestedOpSlotsAligned) {
  CollectionHelper helper;
  auto request = localReq(helper.getSearchEngine());
  auto& cursor = request->facet("tiers", "brand_s");
  auto& facet = queryFacet(cursor);
  auto* buckets = addBuckets(facet, cursor.mr(), 2);
  setBucket(buckets[0], cursor.mr(), "all", qb::all());
  setBucket(buckets[1], cursor.mr(), "none",
            qb::match(cursor.mr(), "brand_s", "none"));
  addExprOp(facet, cursor.mr(), "avg_price", "avg(price_i)", 2);
  addFieldFacetOp(facet, cursor.mr(), "brands", "brand_s", 2);
  request->execute();

  ASSERT_TRUE(request->ok()) << request->errorMsg();
  const auto& result = *request->responses[0]->proto.ops.at("tiers")->facetResult();
  EXPECT_EQ((std::vector<std::pair<std::string, int64_t>>{
                {"all", 0}, {"none", 0}}), queryRows(result));
  const auto& avgs = std::get<api::ArrVal>(
      result.ops.at("avg_price")->kind).v;
  ASSERT_EQ(2u, avgs.size());
  EXPECT_TRUE(avgs[0].isNull());
  EXPECT_TRUE(avgs[1].isNull());
  const auto& brands = std::get<api::ArrVal>(result.ops.at("brands")->kind).v;
  ASSERT_EQ(2u, brands.size());
  for (const api::Val& value : brands) {
    const auto& child = std::get<api::FacetResult>(value.kind);
    EXPECT_TRUE(std::get<api::ColStr>(child.bucket_ids->kind).v.empty());
    EXPECT_TRUE(child.counts.empty());
  }
}

TEST_F(QueryFacetParserTest, RandomTermAndRangeBucketsMatchDocumentOracle) {
  struct ModelDoc {
    int tag;
    int64_t price;
    bool keep;
  };
  struct BucketSpec {
    bool term;
    int tag;
    int64_t low;
    int64_t high;
  };

  std::mt19937 random(0x51a7u);
  CollectionHelper helper;
  std::vector<ModelDoc> docs;
  for (int i = 0; i < 60; i++) {
    ModelDoc doc{
      .tag = (int)(random() % 4),
      .price = (int64_t)(random() % 100),
      .keep = (random() & 1) != 0,
    };
    docs.push_back(doc);
    std::string tag = "t" + std::to_string(doc.tag);
    helper.index(
        flatdoc("id", std::to_string(i), "tag_s", tag,
                "scope_s", doc.keep ? "keep" : "drop",
                "price_i", doc.price),
        i == 59 || i % 13 == 12
            ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  for (int iteration = 0; iteration < 30; iteration++) {
    bool filtered = (random() & 1) != 0;
    size_t bucketCount = 1 + random() % 3;
    auto request = localReq(helper.getSearchEngine());
    auto& top = request->topDocs("q").allQuery();
    if (filtered) top.matchFilter("scope_s", "keep");
    auto& cursor = top.facet("random", "tag_s");
    auto& facet = queryFacet(cursor);
    auto* buckets = addBuckets(facet, cursor.mr(), bucketCount);
    std::vector<BucketSpec> specs;
    for (size_t bucket = 0; bucket < bucketCount; bucket++) {
      BucketSpec spec{};
      spec.term = (random() & 1) != 0;
      spec.tag = (int)(random() % 4);
      spec.low = (int64_t)(random() % 80);
      spec.high = spec.low + 1 + (int64_t)(random() % 30);
      specs.push_back(spec);
      std::string name = "b" + std::to_string(bucket);
      if (spec.term) {
        std::string tag = "t" + std::to_string(spec.tag);
        setBucket(buckets[bucket], cursor.mr(), name,
                  qb::match(cursor.mr(), "tag_s", tag));
      } else {
        setBucket(buckets[bucket], cursor.mr(), name,
                  qb::range(cursor.mr(), "price_i",
                            qb::valI64(cursor.mr(), spec.low), nullptr,
                            nullptr, qb::valI64(cursor.mr(), spec.high)));
      }
    }
    request->execute();
    ASSERT_TRUE(request->ok())
        << "iteration=" << iteration << " " << request->errorMsg();

    std::vector<std::pair<std::string, int64_t>> expected;
    for (size_t bucket = 0; bucket < specs.size(); bucket++) {
      int64_t count = 0;
      for (const ModelDoc& doc : docs) {
        if (filtered && !doc.keep) continue;
        const BucketSpec& spec = specs[bucket];
        if ((spec.term && doc.tag == spec.tag)
            || (!spec.term && doc.price >= spec.low
                               && doc.price < spec.high)) {
          count++;
        }
      }
      expected.emplace_back("b" + std::to_string(bucket), count);
    }
    EXPECT_EQ(expected,
              queryRows(*request->docList("q")->ops.at("random")->facetResult()))
        << "iteration=" << iteration << " filtered=" << filtered;
  }
}

TEST_F(QueryFacetParserTest, PreparedBucketQueriesAreRejectedClearly) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& vectors = schema.templ("_v");
  vectors.type = api::FieldDef::FieldClass::VECTOR;
  vectors.column = true;
  vectors.metric = api::VectorMetric::IP;
  schema.set(helper.collection());
  helper.index(flatdoc("id", "1", "embedding_v",
                       std::vector<float>{1.0f, 0.0f}),
               UpdateMessage::COMMIT);

  auto request = localReq(helper.getSearchEngine());
  auto& cursor = request->topDocs("q").allQuery().facet("near", "brand_s");
  auto& facet = queryFacet(cursor);
  auto* buckets = addBuckets(facet, cursor.mr(), 1);
  setBucket(buckets[0], cursor.mr(), "nearest",
            qb::knn(cursor.mr(), "embedding_v", {1.0f, 0.0f}, 1));
  expectError(*request,
              "query_facet buckets with prepared queries not implemented yet");
}

} // namespace
