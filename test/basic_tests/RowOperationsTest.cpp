// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/NestedTestData.h"
#include "test/QueryBuild.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/query/ParseContext.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/ToParentQuery.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/JsonResponse.h"

namespace luxir::test {
namespace {
class RowOperationsTest : public LuxirTest {
protected:
  CollectionHelper helper;

  void SetUp() override {
    LuxirTest::SetUp();
    nested_test::setSchema(helper.collection());
    for (auto doc : nested_test::DOCS) {
      CollectionHelper::UpdateBuilder input;
      nested_test::add(input, doc); input.commit();
      ASSERT_TRUE(helper.submit(input).success);
    }
  }

  glz::generic_i64 run(std::string_view text, api::DocFormat format = api::DocFormat::ROWS) {
    auto req = localReq(helper.getSearchEngine());
    EXPECT_TRUE(api::read_json(req->rawRequest(), text, req->mr));
    for (const auto& [name, spec] : req->rawRequest().ops) {
      auto& top = const_cast<api::TopDocs&>(std::get<api::TopDocs>(spec->kind));
      top.document_format = format;
      for (const auto& [key, row] : top.row_ops) {
        auto& inner = const_cast<api::TopDocs&>(std::get<api::TopDocs>(row->kind));
        if (inner.document_format == api::DocFormat::DEFAULT) inner.document_format = format;
      }
    }
    req->execute(1); EXPECT_OK(req);
    glz::generic_i64 result;
    EXPECT_FALSE(glz::read_json(result, renderSearchResponseBody(req->responses.back()->proto)));
    return result;
  }

  void error(std::string_view text, std::string_view expected) {
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), text, req->mr));
    req->execute(1);
    EXPECT_FALSE(req->ok()); EXPECT_NE(std::string::npos, req->errorMsg().find(expected)) << req->errorMsg();
  }
};

TEST_F(RowOperationsTest, matchedAndRestatedRowsComposeWithFiltersAndAnExplicitQuery) {
  // Run over one segment per document, then over one merged segment, where
  // one scorer serves every family of the page; a descending page returns
  // rows against row order.
  for (bool merged : {false, true}) for (auto dir : {"asc", "desc"}) {
  if (merged) {
    CollectionHelper::UpdateBuilder merge; merge.commit(true, 1);
    ASSERT_TRUE(helper.submit(merge).success);
  }
  for (auto clause : {
      R"JSON({"name":"m","match":{"field":"items.color","val":"red"}})JSON",
      R"JSON({"name":"m","expr":"items:(color:red)"})JSON",
      R"JSON({"name":"m","to_parent":{"path":"items","query":{"match":{"field":"color","val":"red"}}}})JSON"}) {
    for (auto format : {api::DocFormat::ROWS, api::DocFormat::COLUMNS, api::DocFormat::ADAPTIVE}) {
      auto request = std::string(R"JSON({"ops":{"q":{"top_docs":{"query":)JSON") + clause + R"JSON(,"fields":["id"],"sort":[{"field":"id","dir":")JSON" + dir + R"JSON("}],"row_ops":{
        "matched":{"domain":{"from_match":"m"},"top_docs":{"fields":["color","size"],"count_limit":-1,"get_scores":true}},
        "restated":{"domain":{"to":"items"},"top_docs":{"query":"color:red","fields":["color","size"],"count_limit":-1,"get_scores":true}},
        "explicit":{"domain":{"from_match":"m","filter":["size:L"]},"top_docs":{"query":{"constant_score":{"score":7,"query":"*:*"}},"fields":["color"],"count_limit":-1,"get_scores":true}},
        "filtered":{"domain":{"from_match":"m","filter":["size:L"]},"top_docs":{"fields":["color"],"count_limit":-1,"get_scores":true}}
      }}}}})JSON";
      auto result = run(request, format);
      auto& hits = result["ops"]["q"]["docs"].get_array();
      ASSERT_EQ(2, hits.size());
      for (auto& hit : hits) {
        EXPECT_EQ(glz::write_json(hit["matched"]).value(), glz::write_json(hit["restated"]).value());
        auto expected = hit["id"].get_string() == "a" ? 0 : 1;
        EXPECT_EQ(expected, hit["explicit"]["found"].get<int64_t>());
        EXPECT_EQ(expected, hit["filtered"]["found"].get<int64_t>());
        if (expected) { EXPECT_EQ(7, hit["explicit"]["docs"][0]["_score_"].as_number()); }
        EXPECT_GT(hit["matched"]["docs"][0]["_score_"].as_number(), 0);
      }
    }
  }
  }
}

TEST_F(RowOperationsTest, descendingAscendingAndSingletonOperationsRunOnlyForThePage) {
  SkipStatsScope stats;
  auto result = run(R"JSON({"ops":{"q":{"top_docs":{"sort":[{"field":"id"}],"offset":1,"limit":1,"fields":["id"],"row_ops":{
    "self":{"top_docs":{"fields":["id"],"count_limit":-1}},
    "parts":{"domain":{"to":"items.parts"},"top_docs":{"fields":["material"],"count_limit":-1}},
    "count":{"domain":{"to":"items"},"top_docs":{"limit":0,"count_limit":-1}}
  }}}}})JSON");
  auto& hit = result["ops"]["q"]["docs"][0];
  EXPECT_EQ("b", hit["id"].get_string());
  EXPECT_EQ("b", hit["self"]["docs"][0]["id"].get_string());
  EXPECT_EQ("steel", hit["parts"]["docs"][0]["material"].get_string());
  EXPECT_EQ(1, hit["count"]["found"].get<int64_t>());
  EXPECT_EQ(3, SkipStats::rowOperationRows);
  EXPECT_EQ(3, SkipStats::rowOperationLeafPlans);
  for (auto to : {"..", "/"}) {
    auto request = std::string(R"JSON({"ops":{"q":{"domain":{"to":"items"},"top_docs":{"fields":["color"],"row_ops":{"owner":{"domain":{"to":")JSON")
        + to + R"JSON("},"top_docs":{"fields":["id"]}}}}}}})JSON";
    auto child = run(request);
    auto& rows = child["ops"]["q"]["docs"].get_array();
    ASSERT_EQ(4, rows.size());
    EXPECT_EQ("a", rows[0]["owner"]["docs"][0]["id"].get_string());
    EXPECT_EQ("a", rows[1]["owner"]["docs"][0]["id"].get_string());
  }
}

TEST_F(RowOperationsTest, limitsOffsetsSortEmptyFamiliesAndLargeFamilies) {
  std::string json = R"JSON({"id":"large","items":[)JSON";
  for (int i = 0; i < 4200; i++) {
    if (i) json += ',';
    json += i % 2 ? R"JSON({"color":"red","size":"S"})JSON" : R"JSON({"color":"blue","size":"L"})JSON";
  }
  json += "]}";
  CollectionHelper::UpdateBuilder update;
  nested_test::add(update, json); update.commit(); ASSERT_TRUE(helper.submit(update).success);
  auto check = [&] {
    auto result = run(R"JSON({"ops":{"q":{"top_docs":{"query":"id:large OR id:c","fields":["id"],"sort":[{"field":"id"}],"row_ops":{
      "children":{"domain":{"to":"items"},"top_docs":{"filter":["color:red"],"sort":[{"field":"size","dir":"desc"}],"limit":3,"offset":2,"count_limit":-1,"fields":["color","size"]}}
    }}}}})JSON");
    auto& rows = result["ops"]["q"]["docs"].get_array();
    ASSERT_EQ(2, rows.size());
    EXPECT_EQ(0, rows[0]["children"]["found"].get<int64_t>());
    EXPECT_EQ(2100, rows[1]["children"]["found"].get<int64_t>());
    EXPECT_EQ(3, rows[1]["children"]["docs"].get_array().size());
  };
  check();
  CollectionHelper::UpdateBuilder change; change.remove("b"); change.commit();
  ASSERT_TRUE(helper.submit(change).success);
  CollectionHelper::UpdateBuilder merge; merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  check();
}

TEST_F(RowOperationsTest, cachedRequiredConstantKeepsNamedMatchingRows) {
  SearchOverridesGuard whole(QueryPrep::disableWholeMembershipPlanForTests);
  QueryPrep::disableWholeMembershipPlanForTests = true;
  CollectionHelper::UpdateBuilder merge; merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  auto cache = std::make_shared<FilterCache>(FilterCacheConfig{
      .maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 2});
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  auto request = R"JSON({"ops":{"q":{"top_docs":{
    "get_scores":true,"count_limit":-1,"fields":["id"],"sort":[{"field":"id"}],
    "query":{"boolean":{"required":[{"match":{"field":"id","val":"a"}},
      {"name":"m","constant_score":{"score":3.25,"query":{
        "to_parent":{"path":"items","score":"none","query":{"match":{"field":"color","val":"red"}}}}}}]}},
    "row_ops":{"matched":{"domain":{"from_match":"m"},"top_docs":{
      "get_scores":true,"count_limit":-1,"fields":["color","size"]}}}
  }}}})JSON";
  cache->resize(0);
  auto off = run(request);
  cache->resize(1024 * 1024);
  auto cold = run(request);
  EXPECT_EQ(glz::write_json(off["ops"]["q"]).value(), glz::write_json(cold["ops"]["q"]).value());
  QueryPrep::disableWholeMembershipPlanForTests = false;
  for (int i = 0; i < 2; i++) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().limit(1).countLimit().rawQuery() = qb::toParent(req->mr, "items",
        qb::match(req->mr, "color", "red"), "none");
    req->execute(1); ASSERT_OK(req);
  }
  QueryPrep::disableWholeMembershipPlanForTests = true;
  auto hits = cache->counters().hits;
  SkipStatsScope stats;
  auto warm = run(request);
  EXPECT_EQ(0, SkipStats::joinChildSuppliers);
  EXPECT_GT(cache->counters().hits, hits);
  EXPECT_EQ(glz::write_json(off["ops"]["q"]).value(), glz::write_json(warm["ops"]["q"]).value());
  const auto& rows = warm["ops"]["q"]["docs"][0]["matched"]["docs"].get_array();
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ("red", rows[0]["color"].get_string());
}

TEST_F(RowOperationsTest, namesDoNotChangeMembershipCacheOrPlanning) {
  auto cache = std::make_shared<FilterCache>(FilterCacheConfig{
      .maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 2});
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  for (int pass = 0; pass < 3; pass++) {
    auto request = std::string(R"JSON({"ops":{"q":{"top_docs":{"limit":0,"count_limit":-1,"query":{)JSON")
        + (pass == 2 ? R"JSON("name":"same",)JSON" : "")
        + R"JSON("match":{"field":"items.color","val":"red"}}}}}})JSON";
    SkipStatsScope stats;
    auto hits = cache->counters().hits;
    auto result = run(request);
    EXPECT_EQ(2, result["ops"]["q"]["found"].get<int64_t>());
    if (pass == 2) {
      EXPECT_EQ(cache->counters().hits, hits);
      EXPECT_EQ((int64_t)helper.getIndexWriter()->snapshots.readers.getReader()->segments().size(),
          SkipStats::canonicalCountFactsRead);
    }
  }
}

TEST_F(RowOperationsTest, clauseMetadataDoesNotChangeTheTreeKeyOrScorerPlan) {
  auto req = localReq(helper.getSearchEngine());
  req->reader = helper.getIndexWriter()->snapshots.readers.getReader();
  req->schema = req->reader->schema();
  ParseView view(*req->schema, nullptr, &req->reader, req.get());
  ParseContext context(req->requestPool, *req->schema, req->arena, CoerceContext{}, "q", nullptr, &view);
  ProtobufQueryParser parser(context);
  api::Query wire; wire.kind.emplace<api::ExprQuery>().q = "items:(color:red AND size:L)";
  auto* ordinary = parser.parse(wire);
  wire.name = "matched";
  auto* named = parser.parse(wire);
  EXPECT_TRUE(ordinary->equals(*named)); EXPECT_EQ(ordinary->hash(), named->hash());
  auto key = [](Query* query) {
    FilterKeyContext context; FilterKeyBuilder out;
    auto scope = query->appendFilterKey(out, context);
    return std::move(out).finishStructural(scope, context);
  };
  EXPECT_EQ(key(ordinary), key(named));
  auto* planning = req->createPlanningContext(*req->reader);
  ordinary->validateLogical(*planning); named->validateLogical(*planning);
  auto* weights = Query::Context::create(&req->arena, *planning);
  MemPool pool;
  auto plan = [&](Query* query) {
    auto* weight = query->createWeight(*weights, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(pool, req->reader->segments()[1]);
    return supplier->resolve(pool, supplier->makePlanContext(Query::Demand::fromLeadCost(1)));
  };
  auto* a = plan(ordinary); auto* b = plan(named);
  EXPECT_EQ(a->cost(), b->cost());
  EXPECT_EQ(a->shape().windowFillClause, b->shape().windowFillClause);
  EXPECT_EQ(a->shape().matchState, b->shape().matchState);
  EXPECT_EQ(a->shape().membershipConsumption, b->shape().membershipConsumption);
}

TEST_F(RowOperationsTest, deepNamedClausesFiltersAndScoresUseTheirOwnView) {
  auto result = run(R"JSON({"ops":{"q":{"top_docs":{"query":"id:a","fields":["id"],"filter":[
    {"name":"deep","expr":"items.parts.material:steel"}],"row_ops":{
    "rows":{"domain":{"from_match":"deep"},"top_docs":{"fields":["material"],"get_scores":true,"count_limit":-1}},
    "boosted":{"domain":{"to":"items"},"top_docs":{"query":{"boost":3,"match":{"field":"color","val":"red"}},"fields":["color"],"get_scores":true}}
  }}}}})JSON");
  auto& hit = result["ops"]["q"]["docs"][0];
  EXPECT_EQ(1, hit["rows"]["found"].get<int64_t>());
  EXPECT_EQ("steel", hit["rows"]["docs"][0]["material"].get_string());
  EXPECT_GT(hit["rows"]["docs"][0]["_score_"].as_number(), 0);
  EXPECT_GT(hit["boosted"]["docs"][0]["_score_"].as_number(), 0);
  auto sameItem = run(R"JSON({"ops":{"q":{"top_docs":{"query":{"name":"same","expr":"items:(color:red AND size:L)"},"fields":["id"],"row_ops":{
    "matches":{"domain":{"from_match":"same"},"top_docs":{"fields":["color","size"],"count_limit":-1,"get_scores":true}}
  }}}}})JSON");
  auto& only = sameItem["ops"]["q"]["docs"].get_array();
  ASSERT_EQ(1, only.size()); EXPECT_EQ("b", only[0]["id"].get_string());
  EXPECT_EQ(1, only[0]["matches"]["found"].get<int64_t>());
  EXPECT_EQ("L", only[0]["matches"]["docs"][0]["size"].get_string());
  auto inner = run(R"JSON({"ops":{"q":{"top_docs":{"query":{"to_parent":{"path":"items","query":{
    "name":"part","expr":"parts.material:steel"}}},"fields":["id"],"row_ops":{
    "parts":{"domain":{"from_match":"part"},"top_docs":{"fields":["material"],"count_limit":-1}}
  }}}}})JSON");
  for (const auto& row : inner["ops"]["q"]["docs"].get_array()) {
    EXPECT_EQ(1, row["parts"]["found"].get<int64_t>());
    EXPECT_EQ("steel", row["parts"]["docs"][0]["material"].get_string());
  }
}

TEST_F(RowOperationsTest, namesAreAcceptedWhereverAQueryIsParsed) {
  auto result = run(R"JSON({"ops":{"q":{"top_docs":{"query":{"name":"a","expr":"items.color:red"},
    "filter":[{"name":"b","expr":"id:a OR id:b"},{"query":{"name":"c","all":true},"except_ops":["f"]}],
    "ops":{
      "f":{"domain":{"filter":[{"name":"d","all":true}]},"field_facet":{"field":"id"}},
      "g":{"domain":{"query":{"name":"e","all":true}},"field_facet":{"field":"id"}},
      "h":{"domain":{"to":"items","filter":[{"name":"f","match":{"field":"color","val":"red"}}]},"field_facet":{"field":"color"}},
      "i":{"query_facet":{"buckets":{"one":{"name":"g","expr":"id:a"}}}}
    }}}}})JSON");
  EXPECT_EQ(2, result["ops"]["q"]["docs"].get_array().size());
}

TEST_F(RowOperationsTest, grandchildListsCanReturnOwnerAndRootRows) {
  auto result = run(R"JSON({"ops":{"q":{"domain":{"to":"items.parts"},"top_docs":{"fields":["material"],"row_ops":{
    "parent":{"domain":{"to":".."},"top_docs":{"fields":["color"],"get_scores":true,"document_format":"columns"}},
    "root":{"domain":{"to":"/"},"top_docs":{"fields":["id"]}},
    "two":{"domain":{"to":"../.."},"top_docs":{"fields":["id"]}}
  }}}}})JSON");
  auto& rows = result["ops"]["q"]["docs"].get_array();
  ASSERT_EQ(3, rows.size());
  for (auto& row : rows) {
    EXPECT_EQ(glz::write_json(row["root"]).value(), glz::write_json(row["two"]).value());
    EXPECT_EQ(0, row["parent"]["docs"][0]["_score_"].as_number());
  }
  EXPECT_EQ("red", rows[0]["parent"]["docs"][0]["color"].get_string());
}

TEST_F(RowOperationsTest, replayRejectsKnnAndAClauseFromAnotherList) {
  SchemaBuilder schema;
  auto& vector = schema.field("items.vector");
  vector.type = api::FieldDef::FieldClass::VECTOR; vector.dims = 2; vector.metric = api::VectorMetric::L2;
  schema.set(helper.collection());
  error(R"JSON({"ops":{"q":{"top_docs":{"query":{"name":"m","knn":{"field":"items.vector","query":[0,0],"k":2}},"row_ops":{
    "inner":{"domain":{"from_match":"m"},"top_docs":{}}
  }}}}})JSON", "'m': kNN and other domain-consuming");
  error(R"JSON({"ops":{"a":{"top_docs":{"query":{"name":"m","expr":"items.color:red"}}},"b":{"top_docs":{"row_ops":{
    "inner":{"domain":{"from_match":"m"},"top_docs":{}}
  }}}}})JSON", "'m': the clause is not part of this list");
}

TEST_F(RowOperationsTest, responseMemoryLimitAccountsForInnerResults) {
  CollectionHelper::UpdateBuilder input;
  std::string doc = R"JSON({"id":"many","items":[)JSON";
  for (int i = 0; i < 2000; i++) doc += i ? ",{}" : "{}";
  nested_test::add(input, doc + "]}"); input.commit(); ASSERT_TRUE(helper.submit(input).success);
  SearchOverridesGuard ceiling(forcedRequestMemoryMaxBytes);
  forcedRequestMemoryMaxBytes = 65536;
  error(R"JSON({"ops":{"q":{"top_docs":{"query":"id:many","fields":["id"],"row_ops":{
    "rows":{"domain":{"to":"items"},"top_docs":{"limit":-1,"fields":["color"]}}
  }}}}})JSON", "returned fields");
}

TEST_F(RowOperationsTest, invalidNamesScopesAndUnsupportedOperationsAreRejected) {
  error(R"JSON({"ops":{"q":{"top_docs":{"fields":["id"],"row_ops":{"id":{"top_docs":{}}}}}}})JSON", "conflicts with a returned field");
  error(R"JSON({"ops":{"q":{"top_docs":{"collapse":{"field":"id"},"row_ops":{"inner":{"top_docs":{}}}}}}})JSON", "row_ops on collapsed");
  for (auto name : {"color", "_score_"}) {
    auto fields = std::string(name) == "color" ? "*" : "color";
    auto request = std::string(R"JSON({"ops":{"q":{"domain":{"to":"items"},"top_docs":{"fields":[")JSON") + fields
        + R"JSON("],"row_ops":{")JSON" + name + R"JSON(":{"top_docs":{}}}}}}})JSON";
    error(request, std::string(name) == "_score_" ? "reserved" : "conflicts");
  }
  error(R"JSON({"ops":{"q":{"top_docs":{"query":{"boolean":{"required":[{"name":"m","all":true},{"name":"m","all":true}]}}}}}})JSON", "duplicate clause name");
  for (auto [clause, message] : {
      std::pair{R"JSON({"name":"m","all":true})JSON", "does not select descendant"},
      std::pair{R"JSON({"all":true})JSON", "unknown clause"},
      std::pair{R"JSON({"boolean":{"prohibited":[{"name":"m","match":{"field":"items.color","val":"red"}}]}})JSON", "prohibited clause"}}) {
    auto request = std::string(R"JSON({"ops":{"q":{"top_docs":{"query":)JSON") + clause
        + R"JSON(,"row_ops":{"inner":{"domain":{"from_match":"m"},"top_docs":{}}}}}}})JSON";
    error(request, message);
  }
  error(R"JSON({"ops":{"q":{"top_docs":{"query":{"name":"m","expr":"items.color:red"},"row_ops":{"x":{"domain":{"to":"items","from_match":"m"},"top_docs":{}}}}}}})JSON", "cannot be combined");
  error(R"JSON({"ops":{"q":{"top_docs":{"row_ops":{"x":{"domain":{"query":"*:*"},"top_docs":{}}}}}}})JSON", "domain.query is not supported yet");
  error(R"JSON({"ops":{"q":{"top_docs":{"row_ops":{"x":{"field_facet":{"field":"id"}}}}}}})JSON", "not supported yet");
}
} // namespace
} // namespace luxir::test
