// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// End-to-end tests of the plugin extension points through the example plugin:
// query types, search ops, extra result columns, HTTP routes, node lifecycle.

#include <gtest/gtest.h>

#include "ExampleQuery.h"
#include "ExampleNodeState.h"
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

void exampleThrowOnNodeStateCreation(bool value);

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
  LocalReqHandle sampleDocs(std::string_view params, api::DocFormat format) {
    auto req = localReq(helper.getSearchEngine());
    req->docFormatDefault = format;
    std::string json = R"({"ops":{"s":{"sample_docs":)" + std::string(params) + "}}}";
    auto text = api::build::arenaStr(req->mr, json);
    EXPECT_TRUE(api::read_json(req->rawRequest(), text, req->mr));
    req->execute();
    return req;
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

TEST_F(PluginExtensionTest, extraColumnRejectsDiscoveredFieldCollisions) {
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "d1", "cat_s", "x", "price_i", 17)},
                              UpdateMessage::COMMIT, true).success);
  for (auto format : {api::DocFormat::COLUMNS, api::DocFormat::ROWS}) {
    SCOPED_TRACE((int)format);
    for (std::string_view fields : {"", R"(,"fields":["*"])", R"(,"fields":["price*"])"}) {
      SCOPED_TRACE(fields);
      auto req = sampleDocs(R"({"ids":["d1"],"rank_names":["price_i"])" + std::string(fields) + "}", format);
      EXPECT_FALSE(req->ok());
      EXPECT_EQ(ErrorKind::INVALID_REQUEST, req->errorKind());
      EXPECT_EQ("Extra column 'price_i' conflicts with a returned field; use an explicit field list"
                " to omit the physical field or select the extra by name", req->errorMsg());
      ASSERT_EQ(1u, req->responses.size());
      EXPECT_TRUE(req->responses[0]->proto.ops.empty());
    }
  }
}

TEST_F(PluginExtensionTest, extraColumnNamesMustBeUniqueAndNotReserved) {
  for (auto format : {api::DocFormat::COLUMNS, api::DocFormat::ROWS}) {
    SCOPED_TRACE((int)format);
    auto duplicate = sampleDocs(R"({"ids":["d1"],"fields":["id"],"rank_names":["_rank","_rank"]})", format);
    EXPECT_FALSE(duplicate->ok());
    EXPECT_EQ(ErrorKind::INTERNAL, duplicate->errorKind());
    EXPECT_EQ("Duplicate extra column '_rank'; extra column names must be unique", duplicate->errorMsg());
    for (std::string_view name : {"_score_", "_version_"}) {
      auto req = sampleDocs(R"({"ids":["d1"],"fields":["id"],"rank_names":[")" + std::string(name) + R"("]})", format);
      EXPECT_FALSE(req->ok());
      EXPECT_EQ(ErrorKind::INVALID_REQUEST, req->errorKind());
      EXPECT_EQ("Extra column '" + std::string(name)
                + "' conflicts with a reserved output name; choose a different extra column name", req->errorMsg());
    }
  }
}

TEST_F(PluginExtensionTest, explicitSelectorKeepsExtraAndOtherFields) {
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "d1", "cat_s", "x", "price_i", 17)},
                              UpdateMessage::COMMIT, true).success);
  for (auto format : {api::DocFormat::COLUMNS, api::DocFormat::ROWS}) {
    SCOPED_TRACE((int)format);
    for (auto [fields, size] : {std::pair{R"(["price_i"])", 1u}, {R"(["price_i","cat_s"])", 2u},
                                {R"(["id"])", 2u}, {R"(["*","price_i"])", 3u}, {R"(["price_i","*"])", 3u}}) {
      SCOPED_TRACE(fields);
      auto req = sampleDocs(R"({"ids":["d1"],"rank_names":["price_i"],"fields":)" + std::string(fields) + "}", format);
      ASSERT_OK(req);
      ASSERT_NE(nullptr, req->docList("s"));
      EXPECT_EQ(format == api::DocFormat::COLUMNS, !req->docList("s")->columns.empty());
      EXPECT_EQ(format == api::DocFormat::ROWS, !req->docList("s")->docs.empty());
      auto docs = req->getDocs("s");
      ASSERT_EQ(1u, docs.size());
      EXPECT_EQ(size, docs[0].size());
      const auto* price = find(docs[0], "price_i");
      ASSERT_NE(nullptr, price);
      EXPECT_EQ(1, std::get<int64_t>(*price));  // the rank, rather than the physical value 17
      if (size == 3 || std::string_view(fields).contains("cat_s")) {
        const auto* category = find(docs[0], "cat_s");
        ASSERT_NE(nullptr, category);
        EXPECT_EQ("x", std::get<std::string>(*category));
      }
    }
  }
}

TEST_F(PluginExtensionTest, httpRoutes) {
  auto* state = luxirNode->pluginState<example::NodeState>("example");
  int routes = state->routes.load();
  auto ping = httpRequest(server->getPort(), http::verb::get, "/_plugins/example/ping?echo=hi");
  EXPECT_EQ(200, ping.result_int()) << ping.body();
  EXPECT_EQ(R"({"pong":"hi"})", ping.body());
  EXPECT_EQ(routes + 1, state->routes.load());
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

TEST(PluginQueryTest, makersReachTheirOwnNodeState) {
  LuxirNode first, second;
  auto* a = first.pluginState<example::NodeState>("example");
  auto* b = second.pluginState<example::NodeState>("example");
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  EXPECT_NE(a, b);
  EXPECT_EQ(nullptr, first.pluginState<example::NodeState>("missing"));
  struct OtherState : PluginNodeState {};
  EXPECT_EQ(nullptr, first.pluginState<OtherState>("example"));
  auto run = [&](LuxirNode& node) {
    CollectionHelper docs(node);
    ASSERT_TRUE(docs.index(flatdoc("id", "d1"), UpdateMessage::COMMIT).success);
    auto req = localReq(node.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), R"x({"ops":{
      "q":{"top_docs":{"query":"doc_ids([d1], within=doc_ids([d1]))","fields":["id"]}},
      "s":{"sample_docs":{"ids":["d1"],"fields":["id"]}}
    }})x", req->mr));
    req->execute();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_EQ(1u, req->getDocs("q").size());
    EXPECT_EQ(1u, req->getDocs("s").size());
  };
  run(first);
  EXPECT_EQ(2, a->queries.load());
  EXPECT_EQ(1, a->ops.load());
  EXPECT_EQ(0, b->queries.load());
  EXPECT_EQ(0, b->ops.load());
  run(second);
  EXPECT_EQ(2, a->queries.load());
  EXPECT_EQ(1, a->ops.load());
  EXPECT_EQ(2, b->queries.load());
  EXPECT_EQ(1, b->ops.load());
}

TEST(PluginQueryTest, offlineQueryContextHasNoNode) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  google::protobuf::Arena arena;
  ParseContext parse(pool, *schema, arena, CoerceContext{}, "offline");
  ProtobufQueryParser parser(parse);
  PluginQueryContext context(parse, "doc_ids", ParamReader("doc_ids", ParamReader::Entries{}),
                             [&](const api::Query& query) { return parser.parse(query); });
  EXPECT_EQ(nullptr, context.node());
  api::Query query;
  ASSERT_TRUE(api::read_json(query, R"({"doc_ids":{"values":["d1"]}})", pool));
  EXPECT_NE(nullptr, parser.parse(query));
}

TEST(PluginQueryTest, nodeStateLifecycle) {
  for (auto mode : {LuxirNode::Mode::SERVE, LuxirNode::Mode::PULL}) {
    std::shared_ptr<std::atomic<bool>> stopped;
    {
      LuxirNode node(LuxirConfig{}, mode);
      auto* state = node.pluginState<example::NodeState>("example");
      ASSERT_NE(nullptr, state);
      EXPECT_EQ(mode, state->mode);
      stopped = state->stopped;
      EXPECT_FALSE(stopped->load());
    }
    EXPECT_TRUE(stopped->load());
  }
}

TEST(PluginQueryTest, nodeConstructorRethrowsStateCreationFailure) {
  {
    exampleThrowOnNodeStateCreation(true);
    auto reset = scope_guard([]() { exampleThrowOnNodeStateCreation(false); });
    try {
      LuxirNode node;
      FAIL();
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ("example node state creation failed", e.what());
    }
  }
  LuxirNode node;
  EXPECT_NE(nullptr, node.pluginState<example::NodeState>("example"));
}
