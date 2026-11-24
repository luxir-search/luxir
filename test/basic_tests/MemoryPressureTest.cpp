// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <memory_resource>
#include <thread>

#include <gtest/gtest.h>

#include "luxir/index/IndexRamBudget.h"
#include "luxir/search/RequestMemTracker.h"
#include "luxir/server/GRPCServer.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/MemoryPressure.h"
#include "luxir/server/RpcStatus.h"
#include "test/CollectionHelper.h"
#include "test/GrpcClient.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"

namespace luxir::test {

class MemoryPressureTest : public LuxirTest {};

namespace {

using State = MemoryPressure::State;
using Work = MemoryPressure::Work;
constexpr int64_t MiB = 1024 * 1024;

// Sampling off: the state moves only by injected samples.
LuxirConfig injectedConfig() {
  LuxirConfig config;
  config.server.memory_pressure_sample_ms = 0;
  return config;
}

MemoryPressure::Sample percent(uint64_t used) { return {used, 100}; }

std::string errorCodeOf(const http::response<http::string_body>& res) {
  glz::generic_i64 root;
  if (glz::read_json(root, res.body())) return {};
  return root["error"]["code"].get_string();
}

}  // namespace

// Soft refuses only new update requests, hard everything new; each level is
// left only below its threshold less the hysteresis. Soft pressure lowers the
// indexing cap below the reservations, and normal restores it.
TEST_F(MemoryPressureTest, thresholdsHysteresisAndFlushHint) {
  ServerConfig server;
  server.memory_pressure_sample_ms = 0;
  IndexRamBudget budget(64 * MiB);
  auto reserved = budget.forceAcquire(16 * MiB);
  MemoryPressure monitor(server, budget, 64 * MiB);

  auto expect = [&](uint64_t used, State state) {
    monitor.injectSampleForTests(percent(used));
    EXPECT_EQ(state, monitor.current()) << used << "%";
  };
  expect(84, State::NORMAL);
  EXPECT_FALSE(monitor.admit(Work::UPDATE));
  expect(85, State::SOFT);
  EXPECT_EQ(8 * MiB, budget.totalBytes());
  EXPECT_FALSE(monitor.admit(Work::SEARCH));
  EXPECT_FALSE(monitor.admit(Work::UPDATE_BATCH));
  auto rejected = monitor.admit(Work::UPDATE);
  ASSERT_TRUE(rejected);
  EXPECT_EQ(ErrorKind::RESOURCE_EXHAUSTED, rejected->kind);
  EXPECT_EQ("memory_pressure", rejected->code);
  expect(91, State::SOFT);
  expect(92, State::HARD);
  EXPECT_TRUE(monitor.admit(Work::SEARCH));
  EXPECT_TRUE(monitor.admit(Work::UPDATE_BATCH));
  expect(88, State::HARD);
  expect(86, State::SOFT);
  expect(81, State::SOFT);
  EXPECT_EQ(8 * MiB, budget.totalBytes());
  expect(79, State::NORMAL);
  EXPECT_EQ(64 * MiB, budget.totalBytes());
  expect(99, State::HARD);
  expect(10, State::NORMAL);

  auto stats = monitor.snapshot();
  EXPECT_EQ(MemoryPressure::Source::INJECTED, stats.source);
  EXPECT_EQ(6u, stats.transitions);
  EXPECT_EQ(1u, stats.rejected[(size_t)Work::UPDATE][(size_t)State::SOFT]);
  EXPECT_EQ(1u, stats.rejected[(size_t)Work::SEARCH][(size_t)State::HARD]);
}

// A live node samples real memory: a source and a limit, and no pressure in a
// test process.
TEST_F(MemoryPressureTest, samplesProcessOrCgroup) {
  LuxirNode node(LuxirConfig{});
  auto stats = node.getMemoryPressure().snapshot();
  EXPECT_NE(MemoryPressure::Source::NONE, stats.source);
  EXPECT_GT(stats.sample.usedBytes, 0u);
  EXPECT_GT(stats.sample.limitBytes, stats.sample.usedBytes);
}

// HTTP: searches and updates get 429 memory_pressure with Retry-After, while
// stats, health and collection deletion keep working.
TEST_F(MemoryPressureTest, httpRejectsNewWork) {
  LuxirNode node(injectedConfig());
  HttpServer server(node, 2, 0);
  server.start();
  int port = server.getPort();
  auto update = [&](std::string_view collection) {
    return httpRequest(port, http::verb::post, "/collections/" + std::string(collection) + "/_update",
                       R"({"docs":[{"id":"1","title_w":"pressure"}],"commit":{}})");
  };
  auto search = [&] {
    return httpRequest(port, http::verb::post, "/collections/main/_search", R"({"query":"title_w:pressure"})");
  };
  auto ndjson = [&] {
    return httpRequest(port, http::verb::post, "/collections/main/_update",
                       "{\"id\":\"2\",\"title_w\":\"pressure\"}\n", "application/x-ndjson");
  };
  auto expectRejected = [](const http::response<http::string_body>& res, std::string_view what) {
    EXPECT_EQ(429, res.result_int()) << what << ": " << res.body();
    EXPECT_EQ("1", res[http::field::retry_after]) << what;
    EXPECT_EQ("memory_pressure", errorCodeOf(res)) << what << ": " << res.body();
  };
  ASSERT_EQ(200, update("main").result_int());
  ASSERT_EQ(200, update("doomed").result_int());

  auto& monitor = node.getMemoryPressure();
  monitor.injectSampleForTests(percent(88));
  expectRejected(update("main"), "soft update");
  expectRejected(ndjson(), "soft ndjson");
  EXPECT_EQ(200, search().result_int());

  monitor.injectSampleForTests(percent(95));
  expectRejected(search(), "hard search");
  expectRejected(update("main"), "hard update");
  auto stats = httpRequest(port, http::verb::get, "/_stats");
  ASSERT_EQ(200, stats.result_int()) << stats.body();
  EXPECT_NE(stats.body().find(R"("state": "hard")"), std::string::npos) << stats.body();
  auto metrics = httpRequest(port, http::verb::get, "/metrics");
  EXPECT_NE(metrics.body().find(R"(luxir_memory_pressure_state{state="hard"} 1)"), std::string::npos);
  EXPECT_NE(metrics.body().find(R"(luxir_admission_rejections_total{work="search",cause="memory_pressure_hard"} 1)"),
            std::string::npos);
  EXPECT_EQ(200, httpRequest(port, http::verb::get, "/health").result_int());
  EXPECT_EQ(200, httpRequest(port, http::verb::post, "/collections/_delete", R"({"name":"doomed"})").result_int());

  monitor.injectSampleForTests(percent(10));
  EXPECT_EQ(200, search().result_int());
  EXPECT_EQ(200, update("main").result_int());
  server.shutdown();
}

// gRPC: a unary update ends with RESOURCE_EXHAUSTED carrying memory_pressure;
// a search reports it in-band; Stats still answers.
TEST_F(MemoryPressureTest, grpcRejectsNewWork) {
  LuxirNode node(injectedConfig());
  CollectionHelper(node).index(flatdoc("id", "1"), UpdateMessage::COMMIT);
  GRPCServer server(node, 2, 0);
  std::thread serverThread([&] { server.run(); });
  ASSERT_TRUE(server.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(server.getPort()),
                                     grpc::InsecureChannelCredentials());
  node.getMemoryPressure().injectSampleForTests(percent(95));

  luxir::api::UpdateRequest update;
  grpc::ClientContext updateContext;
  Reply<luxir::api::UpdateResponse> updateReply;
  auto status = hppUnaryCall(channel.get(), rpc::Update, &updateContext, update, &updateReply);
  EXPECT_EQ(grpc::StatusCode::RESOURCE_EXHAUSTED, status.error_code()) << status.error_message();
  std::pmr::monotonic_buffer_resource arena;
  luxir::api::Error detail;
  ASSERT_TRUE(decodeRpcStatusDetails(status.error_details(), detail, arena));
  EXPECT_EQ("memory_pressure", detail.code);

  TrivialSearchRequest search;
  grpc::ClientContext searchContext;
  HppClientReaderWriter<luxir::api::SearchRequest, luxir::api::SearchResponse> stream(
      channel.get(), rpc::Search, &searchContext);
  ASSERT_TRUE(stream.Write(search.request));
  ASSERT_TRUE(stream.WritesDone());
  Reply<luxir::api::SearchResponse> response;
  ASSERT_TRUE(stream.Read(&response));
  ASSERT_TRUE(response.msg.error.has_value());
  EXPECT_EQ(luxir::api::Error::Kind::RESOURCE_EXHAUSTED, response.msg.error->kind);
  EXPECT_EQ("memory_pressure", response.msg.error->code);
  while (stream.Read(&response)) {}
  EXPECT_TRUE(stream.Finish().ok());

  luxir::api::StatsRequest stats;
  grpc::ClientContext statsContext;
  Reply<luxir::api::StatsResponse> statsReply;
  EXPECT_TRUE(hppUnaryCall(channel.get(), rpc::Stats, &statsContext, stats, &statsReply).ok());
  ASSERT_TRUE(statsReply.msg.memory.has_value());
  EXPECT_EQ("hard", statsReply.msg.memory->pressure.state);
  EXPECT_EQ(1u, statsReply.msg.memory->pressure.rejected_searches);

  server.shutdown();
  serverThread.join();
}

// Trackers of concurrent requests share the node-wide ceiling: a charge that
// would pass it fails retryably, and a release makes room again.
TEST_F(MemoryPressureTest, nodeRequestMemoryBoundsRequestsTogether) {
  NodeRequestMemory node(100);
  RequestMemTracker a(0, &node), b(0, &node);
  a.charge(60, "test", "a");
  try {
    b.charge(50, "test", "b");
    FAIL() << "charged past the node ceiling";
  } catch (const ApiError& e) {
    EXPECT_EQ("node_request_memory_exceeded", e.code);
    EXPECT_EQ(ErrorKind::RESOURCE_EXHAUSTED, e.kind);
  }
  EXPECT_EQ(0u, b.bytes());
  EXPECT_FALSE(b.tryCharge(50));
  EXPECT_EQ(30u, b.chargeUpTo(80, 30, "test", "b"));  // the node grants the minimum
  EXPECT_EQ(90u, node.bytes());
  a.release(60);
  b.charge(50, "test", "b");
  EXPECT_EQ(80u, node.bytes());
  b.release(80);
  EXPECT_EQ(0u, node.bytes());
  EXPECT_EQ(1u, node.rejected());
}

// End to end: a search whose tracked memory passes the node ceiling fails
// with node_request_memory_exceeded and leaves nothing charged.
TEST_F(MemoryPressureTest, nodeRequestMemoryRejectsSearch) {
  LuxirConfig config;
  config.search.node_request_memory_max_bytes = 1;
  LuxirNode node(config);
  CollectionHelper helper(node);
  helper.indexAll({flatdoc("id", "1", "brand_s", "acme"), flatdoc("id", "2", "brand_s", "beta")},
                  UpdateMessage::COMMIT);
  auto req = localReq(node.getSearchEngine());
  auto& top = req->collection("main").topDocs("q").allQuery().limit(0);
  top.facet("brands", "brand_s").limit(-1);
  top.filter(qb::match(top.mr(), "brand_s", "acme"), {"brands"});
  ExpectLog quiet("Search request rejected");
  req->execute();
  EXPECT_EQ("node_request_memory_exceeded", req->errorCode()) << req->errorMsg();
  EXPECT_EQ(0u, node.getSearchEngine().nodeRequestMemory().bytes());
}

}  // namespace luxir::test
