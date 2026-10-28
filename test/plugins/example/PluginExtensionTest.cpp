// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// End-to-end tests of the plugin extension points through the example plugin:
// query types, search ops, extra result columns, HTTP routes, node lifecycle.

#include <gtest/gtest.h>

#include "ExampleQuery.h"
#include "luxir/plugin/Registries.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

int exampleNodesStarted();
int exampleNodesStopped();
int examplePullNodesStarted();
int examplePullNodesStopped();
void exampleThrowOnNodeStart(bool value);

using namespace luxir;
using namespace luxir::test;

namespace {

class PluginExtensionTest : public LuxirTest {
protected:
  CollectionHelper helper;
  std::unique_ptr<HttpServer> server;

  void SetUp() override {
    initPlugins(addConfiguredPlugins);
    helper.clear();
    helper.indexAll({flatdoc("id", "d1", "cat_s", "x"), flatdoc("id", "d2", "cat_s", "y")}, UpdateMessage::COMMIT);
    helper.indexAll({flatdoc("id", "d3", "cat_s", "x"), flatdoc("id", "d4", "cat_s", "x")}, UpdateMessage::COMMIT);
    helper.deleteById("d4", UpdateMessage::COMMIT);
    server = std::make_unique<HttpServer>(*LuxirTest::luxirNode, 1, 0);
    server->start();
  }
  void TearDown() override { server->shutdown(); }

  http::response<http::string_body> post(std::string_view target, std::string body) {
    return httpRequest(server->getPort(), http::verb::post, target, std::move(body));
  }
  http::response<http::string_body> search(std::string body) {
    return post("/collections/main/_search", std::move(body));
  }
  // found for a filter-only search over `query` (a JSON query value).
  std::string found(const std::string& query) {
    auto res = search(R"({"query":)" + query + R"(,"fields":["id"],"count_limit":-1})");
    EXPECT_EQ(200, res.result_int()) << query << "\n" << res.body();
    auto at = res.body().find(R"("found":)");
    if (at == std::string::npos) return "none";
    return res.body().substr(at + 8, res.body().find_first_of(",}", at + 8) - at - 8);
  }
};

void noop(HttpPluginRequest&, HttpPluginResponse&) {}

}  // namespace

TEST(PluginRegistryTest, queryTypesValidateNames) {
  QueryRegistry r;
  auto maker = [](PluginQueryContext&) -> Query* { return nullptr; };
  r.add("my_query", maker);
  EXPECT_THROW(r.add("my_query", maker), std::invalid_argument);
  EXPECT_THROW(r.add("match", maker), std::invalid_argument);   // a built-in arm
  EXPECT_THROW(r.add("plugin", maker), std::invalid_argument);
  EXPECT_THROW(r.add("Bad-Name", maker), std::invalid_argument);
  EXPECT_THROW(r.add("no_maker", QueryMaker{}), std::invalid_argument);
  EXPECT_THROW(r.add("bad_positional", PluginQueryDef{maker, {}, "x"}), std::invalid_argument);
  EXPECT_NE(nullptr, r.find("my_query"));
  try {
    r.require("nope");
    FAIL();
  } catch (const RequestError& e) {
    EXPECT_NE(std::string::npos, std::string(e.what()).find("registered plugin query types: my_query")) << e.what();
  }
  r.freeze();
  EXPECT_THROW(r.add("later", maker), std::logic_error);
}

TEST(PluginRegistryTest, searchOpsValidateNames) {
  SearchOpRegistry r;
  auto maker = [](PluginOpContext&) -> SearchOp* { return nullptr; };
  r.add("my_op", maker);
  EXPECT_THROW(r.add("my_op", maker), std::invalid_argument);
  EXPECT_THROW(r.add("top_docs", maker), std::invalid_argument);
  EXPECT_THROW(r.add("domain", maker), std::invalid_argument);
  EXPECT_THROW(r.add("", maker), std::invalid_argument);
  EXPECT_THROW(r.add("no_maker", SearchOpMaker{}), std::invalid_argument);
  EXPECT_THROW(r.require("nope"), RequestError);
  r.freeze();
  EXPECT_THROW(r.add("later", maker), std::logic_error);
}

TEST(PluginRegistryTest, httpRoutesValidatePatternsAndMatch) {
  HttpRouteRegistry r;
  r.add("GET", "/_plugins/p/items/{item}", noop);
  r.add("POST", "/_plugins/p/items/{item}", noop, true);
  r.add("POST", "/collections/{collection}/_graph/paths", noop);
  EXPECT_THROW(r.add("GET", "/_plugins/p/items/{item}", noop), std::invalid_argument);   // duplicate
  EXPECT_THROW(r.add("PATCH", "/_plugins/p/x", noop), std::invalid_argument);
  EXPECT_THROW(r.add("GET", "/elsewhere/x", noop), std::invalid_argument);
  EXPECT_THROW(r.add("GET", "/collections/{collection}/x/_search", noop), std::invalid_argument);
  EXPECT_THROW(r.add("GET", "/collections/{collection}/_snapshot/x", noop), std::invalid_argument);
  EXPECT_THROW(r.add("GET", "/collections/{c}/_x", noop), std::invalid_argument);
  EXPECT_THROW(r.add("GET", "/_plugins/{p}/x", noop), std::invalid_argument);
  EXPECT_THROW(r.add("GET", "/_plugins/p/x", HttpHandler{}), std::invalid_argument);
  // Overlapping patterns: a capture against another capture or a literal.
  EXPECT_THROW(r.add("GET", "/_plugins/p/items/{other}", noop), std::invalid_argument);
  EXPECT_THROW(r.add("GET", "/_plugins/p/items/all", noop), std::invalid_argument);
  EXPECT_THROW(r.add("GET", "/collections/{collection}/_graph/{what}", noop), std::invalid_argument);
  r.add("GET", "/_plugins/p/items/{item}/tags", noop);  // a different segment count is fine
  r.add("GET", "/_plugins/p/other/{item}", noop);       // literals differ at one position

  auto m = r.match("/_plugins/p/items/42");
  ASSERT_TRUE(m);
  EXPECT_EQ(2u, m->routes.size());
  EXPECT_EQ("GET, POST", m->allow);
  ASSERT_EQ(1u, m->params.size());
  EXPECT_EQ("item", m->params[0].first);
  EXPECT_EQ("42", m->params[0].second);
  auto c = r.match("/collections/books/_graph/paths");
  ASSERT_TRUE(c);
  EXPECT_EQ("books", c->params[0].second);
  EXPECT_FALSE(r.match("/_plugins/p/items"));
  EXPECT_FALSE(r.match("/_plugins/p/items/42/more"));
}

TEST_F(PluginExtensionTest, queryTypeInJsonAndExpr) {
  EXPECT_EQ("2", found(R"({"doc_ids":{"values":["d1","d2","d4"]}})"));  // d4 is deleted
  EXPECT_EQ("1", found(R"({"doc_ids":{"values":["d1","d2"],"within":{"match":{"cat_s":"x"}}}})"));
  EXPECT_EQ("2", found(R"({"doc_ids":{"values":["d1","d2","d3"],"within":"cat_s:x"}})"));
  EXPECT_EQ("1", found(R"({"plugin":{"name":"doc_ids","params":{"values":"d3"}}})"));
  EXPECT_EQ("1", found(R"({"boolean":{"required":[{"doc_ids":{"values":["d1","d3"]}}],"prohibited":[{"match":{"id":"d3"}}]}})"));
  EXPECT_EQ("2", found(R"x("doc_ids([d1, d2])")x"));
  EXPECT_EQ("2", found(R"x("doc_ids([d1, d2, d3], within=cat_s:x)")x"));
  EXPECT_EQ("1", found(R"x("doc_ids(values=[d1, d2, d3], within=(cat_s:x AND id:d3))")x"));
  EXPECT_EQ("2", found(R"x("doc_ids([d2]) OR id:d1")x"));
}

TEST_F(PluginExtensionTest, queryTypeErrors) {
  auto expectError = [&](const std::string& query, std::string_view text) {
    auto res = search(R"({"query":)" + query + "}");
    EXPECT_EQ(400, res.result_int()) << query << "\n" << res.body();
    EXPECT_NE(std::string::npos, res.body().find(text)) << query << "\n" << res.body();
  };
  expectError(R"({"plugin":{"name":"nope"}})", "unknown query type 'nope'");
  expectError(R"({"doc_ids":{"vals":["d1"]}})", "doc_ids: unknown parameter 'vals'; valid: values, within");
  expectError(R"({"doc_ids":{"values":[1,2]}})", "doc_ids: 'values' must be a string or an array of strings");
  expectError(R"({"doc_ids":"d1"})", "a plugin arm takes an object of parameters");
  expectError(R"({"doc_ids":{"values":["d1"],"values":["d2"]}})", "duplicate plugin parameter");
  expectError(R"({"not_registered":{}})", "unknown_key");
  expectError(R"x("doc_ids(vals=[d1])")x", "unknown argument 'vals' for doc_ids()");
  expectError(R"({"unscoped_value":{"value":"d1"}})",
              "query type 'unscoped_value' returned a PLUGIN query that must derive from PluginQueryBase");
  expectError(R"x("unscoped_value(d1)")x",
              "query type 'unscoped_value' returned a PLUGIN query that must derive from PluginQueryBase");
}

TEST(PluginQueryTest, identityAndSharedFilterCache) {
  LuxirNode node;
  CollectionHelper helper(node, "plugin_identity");
  FilterCacheConfig config;
  config.minSegmentDocs = 0;
  config.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(config);
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "x", "cat_s", "y"), flatdoc("id", "y", "cat_s", "x")},
                              UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  MemPool pool;
  google::protobuf::Arena arena;
  ParseContext parseContext(pool, *reader->schema(), arena, CoerceContext{}, "");
  ProtobufQueryParser parser(parseContext);
  auto parse = [&](std::string_view json) {
    api::Query wire;
    EXPECT_TRUE(api::read_json(wire, json, pool));
    return parser.parse(wire);
  };
  auto* category = parse(R"({"field_value":{"value":"x"}})");
  auto* same = parse(R"x("field_value(x)")x");
  auto* byId = parse(R"({"field_value":{"value":"x","by_id":true}})");
  auto* alias = parse(R"({"field_value_alias":{"value":"x"}})");
  auto* different = parse(R"({"field_value":{"value":"y"}})");
  ASSERT_EQ(QueryKind::PLUGIN, category->getKind());
  EXPECT_NE(nullptr, dynamic_cast<example::CategoryValueQuery*>(category));
  EXPECT_NE(nullptr, dynamic_cast<example::IdValueQuery*>(byId));
  EXPECT_TRUE(category->equals(*same));
  EXPECT_EQ(category->hash(), same->hash());
  for (auto* other : {byId, alias, different}) {
    EXPECT_FALSE(category->equals(*other));
    EXPECT_FALSE(other->equals(*category));
    EXPECT_NE(category->hash(), other->hash());
  }
  const auto& [name, def] = QueryRegistry::global().require("field_value");
  EXPECT_EQ(name.data(), static_cast<PluginQueryBase*>(category)->registeredName().data());
  EXPECT_EQ(name.data(), static_cast<PluginQueryBase*>(same)->registeredName().data());

  const void* categoryEntry = nullptr;
  // Each Context is a separate request: the second query must hit the shared
  // cache, and the other class must materialize its own distinct document.
  Query* queries[] = {category, same, byId, alias};
  for (int i = 0; i < 4; i++) {
    MemPool executionPool;
    Query::Context context(executionPool, *reader);
    queries[i]->validateLogical(context.planningContext());
    auto* use = context.getFilterUse(*queries[i]);
    ASSERT_NE(nullptr, use);
    ASSERT_NE(nullptr, use->entryIdentityForTest());
    if (i == 0) categoryEntry = use->entryIdentityForTest();
    if (i == 1) {
      EXPECT_EQ(categoryEntry, use->entryIdentityForTest());
    }
    if (i >= 2) {
      EXPECT_NE(categoryEntry, use->entryIdentityForTest());
    }
    auto docs = QueryPrep::materializeEffectiveFilter(
        *queries[i]->createWeight(context, 0), use, *reader, reader->segments()[0], nullptr);
    ASSERT_NE(nullptr, docs.get());
    EXPECT_EQ(1, docs.get()->card());
    EXPECT_TRUE(docs.get()->get(i == 2 ? 0 : 1));
    EXPECT_FALSE(docs.get()->get(i == 2 ? 1 : 0));
    EXPECT_EQ((uint64_t)(i == 0 ? 1 : i), cache->counters().builds);
  }
  EXPECT_EQ(3u, cache->entryCountForTest());
  EXPECT_GT(cache->counters().hits, 0u);

  auto req = localReq(helper.getSearchEngine());
  auto& op = req->collection("plugin_identity").topDocs("q").allQuery().countLimit();
  api::Query filter;
  ASSERT_TRUE(api::read_json(filter, R"({"field_value":{"value":"x","by_id":true}})", req->mr));
  op.filter(filter).fields({"id"});
  req->execute();
  ASSERT_OK(req);
  EXPECT_EQ(1, req->getMatchCount());
  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  const auto* id = find(docs[0], "id");
  ASSERT_NE(nullptr, id);
  EXPECT_EQ("x", std::get<std::string>(*id));
}

TEST_F(PluginExtensionTest, explainRoundTripsPluginArms) {
  auto res = post("/collections/main/_search?explain=request",
                  R"({"query":{"doc_ids":{"values":["d1"]}},"ops":{"s":{"sample_docs":{"ids":["d1"]}}}})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(std::string::npos, res.body().find(R"("plugin":{"name":"doc_ids","params":{"values":["d1"]}})")) << res.body();
  EXPECT_NE(std::string::npos, res.body().find(R"("plugin":{"name":"sample_docs","params":{"ids":["d1"]}})")) << res.body();
}

TEST_F(PluginExtensionTest, searchOpWithExtraColumnAndNestedOps) {
  auto res = search(R"({"ops":{"s":{"sample_docs":{"ids":["d3","d4","d1"],"fields":["id","_rank"],)"
                    R"("ops":{"cats":{"field_facet":{"field":"cat_s"}}}}}}})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  const std::string& body = res.body();
  auto d3 = body.find(R"({"id":"d3","_rank":1})");
  auto d1 = body.find(R"({"id":"d1","_rank":2})");
  EXPECT_NE(std::string::npos, d3) << body;
  EXPECT_NE(std::string::npos, d1) << body;
  EXPECT_LT(d3, d1) << body;
  EXPECT_EQ(std::string::npos, body.find(R"("d4")")) << body;
  EXPECT_NE(std::string::npos, body.find(R"("found":2)")) << body;
  // Both sampled docs have cat_s x; the facet ran over exactly them.
  EXPECT_NE(std::string::npos, body.find(R"("cats":{"buckets":[{"val":"x","count":2}]})")) << body;

  auto bad = search(R"({"ops":{"s":{"sample_docs":{"ids":["d1"],"ops":{"c":{"field_facet":{"field":"cat_s"},)"
                    R"("domain":{"query":{"all":true}}}}}}}})");
  EXPECT_EQ(400, bad.result_int()) << bad.body();
  EXPECT_NE(std::string::npos, bad.body().find("domain is not supported under a plugin operation's ops")) << bad.body();
  auto unknown = search(R"({"ops":{"s":{"plugin":{"name":"nope"}}}})");
  EXPECT_EQ(400, unknown.result_int()) << unknown.body();
  // Out-of-tree plugins may register more operations beside the example's.
  EXPECT_NE(std::string::npos, unknown.body().find("unknown search op 'nope'; registered plugin operations: ")) << unknown.body();
  EXPECT_NE(std::string::npos, unknown.body().find("sample_docs")) << unknown.body();
}

TEST_F(PluginExtensionTest, httpRoutes) {
  auto ping = httpRequest(server->getPort(), http::verb::get, "/_plugins/example/ping?echo=hi");
  EXPECT_EQ(200, ping.result_int()) << ping.body();
  EXPECT_EQ(R"({"pong":"hi"})", ping.body());
  auto wrongMethod = post("/_plugins/example/ping", "");
  EXPECT_EQ(405, wrongMethod.result_int()) << wrongMethod.body();
  EXPECT_EQ("GET", std::string(wrongMethod[http::field::allow]));
  auto missing = httpRequest(server->getPort(), http::verb::get, "/_plugins/example/nope");
  EXPECT_EQ(404, missing.result_int()) << missing.body();

  auto sample = post("/collections/main/_sample", R"({"ids":["d2"],"fields":["id"]})");
  EXPECT_EQ(200, sample.result_int()) << sample.body();
  EXPECT_NE(std::string::npos, sample.body().find(R"({"id":"d2","_rank":1})")) << sample.body();
  auto badSample = post("/collections/main/_sample", R"({"idz":["d2"]})");
  EXPECT_EQ(400, badSample.result_int()) << badSample.body();
  EXPECT_NE(std::string::npos, badSample.body().find("unknown parameter 'idz'")) << badSample.body();
}

TEST_F(PluginExtensionTest, nodeLifecycle) {
  EXPECT_GE(exampleNodesStarted(), 1);
  int started = exampleNodesStarted(), stopped = exampleNodesStopped();
  {
    LuxirNode node;
    EXPECT_EQ(LuxirNode::Mode::SERVE, node.getMode());
  }
  EXPECT_EQ(started + 1, exampleNodesStarted());
  EXPECT_EQ(stopped + 1, exampleNodesStopped());
  int pullStarted = examplePullNodesStarted(), pullStopped = examplePullNodesStopped();
  {
    LuxirNode node(LuxirConfig{}, LuxirNode::Mode::PULL);
    EXPECT_EQ(LuxirNode::Mode::PULL, node.getMode());
  }
  EXPECT_EQ(started + 2, exampleNodesStarted());
  EXPECT_EQ(stopped + 2, exampleNodesStopped());
  EXPECT_EQ(pullStarted + 1, examplePullNodesStarted());
  EXPECT_EQ(pullStopped + 1, examplePullNodesStopped());
}

TEST_F(PluginExtensionTest, nodeConstructorRethrowsHookFailure) {
  int started = exampleNodesStarted(), stopped = exampleNodesStopped();
  {
    exampleThrowOnNodeStart(true);
    auto reset = scope_guard([]() { exampleThrowOnNodeStart(false); });
    try {
      LuxirNode node;
      FAIL();
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ("example node start failed", e.what());
    }
  }
  EXPECT_EQ(started, exampleNodesStarted());
  EXPECT_EQ(stopped, exampleNodesStopped());
  { LuxirNode node; }
  EXPECT_EQ(started + 1, exampleNodesStarted());
  EXPECT_EQ(stopped + 1, exampleNodesStopped());
}
