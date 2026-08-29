#include <gtest/gtest.h>

#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

#include "luxir/api/build.h"
#include "luxir/api/luxir_types.hpp"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
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
    helper->index(flatdoc("id", "1", "brand_s", "acme"),
                  UpdateMessage::COMMIT);
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

TEST_F(QueryFacetParserTest, ValidationPrecedesExecutionGate) {
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
    auto request = localReq(luxirNode->getSearchEngine());
    auto& cursor = request->topDocs("q").allQuery().facet("prices", "brand_s");
    auto& facet = addValidFacet(cursor);
    setSelected(facet, cursor.mr(), {"cheap", "mid"});
    facet.selection_mode = api::SelectionMode::ALL;
    expectError(*request, "query_facet execution not implemented yet");
  }
}

} // namespace
