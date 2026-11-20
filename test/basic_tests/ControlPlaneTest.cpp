// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <thread>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include "luxir/server/GRPCServer.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/Signal.h"
#include "test/GrpcClient.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"

namespace luxir::test {

using namespace std::chrono_literals;
static constexpr auto requestTimeout = 30s;

class ControlPlaneTest : public LuxirTest {
protected:
  static auto request(int port, http::verb method, std::string_view target, std::string body = {}) {
    return httpRequest(port, method, target, std::move(body), "application/json", requestTimeout);
  }
};

// Holds every search-arena worker, as long searches would. A canary queued
// behind them proves nothing else ran there meanwhile.
class SaturatedSearchArena {
  LuxirNode& node;
  std::atomic<int> started{0};
  std::promise<void> allStarted;
  std::atomic<bool> released{false};
  std::latch finished;

public:
  std::atomic<bool> canaryRan{false};

  explicit SaturatedSearchArena(LuxirNode& node) : node(node), finished(node.getWorkers() + 1) {
    for (int i = 0; i < node.getWorkers(); i++) {
      node.getTaskArena().enqueue([this] {
        if (++started == this->node.getWorkers()) allStarted.set_value();
        released.wait(false);
        finished.count_down();
      });
    }
    EXPECT_EQ(std::future_status::ready, allStarted.get_future().wait_for(requestTimeout));
    node.getTaskArena().enqueue([this] { canaryRan = true; finished.count_down(); });
  }
  ~SaturatedSearchArena() { release(); finished.wait(); }
  void release() { released = true; released.notify_all(); }
};

static LuxirConfig twoWorkers() {
  LuxirConfig config;
  config.server.workers = 2;
  return config;
}

// Stats, metrics, collection lifecycle, schema changes and buffered updates
// all answer while every search worker is held.
TEST_F(ControlPlaneTest, httpControlPlaneAnswersWhileSearchesHoldEveryWorker) {
  LuxirNode node(twoWorkers());
  HttpServer server(node, 2, 0);
  server.start();
  auto port = server.getPort();
  {
    SaturatedSearchArena busy(node);
    auto stats = request(port, http::verb::get, "/_stats");
    EXPECT_EQ(200, stats.result_int());
    EXPECT_NE(std::string::npos, stats.body().find("\"control_threads\": 2"));
    auto metrics = request(port, http::verb::get, "/metrics");
    ASSERT_EQ(200, metrics.result_int());
    EXPECT_NE(std::string::npos, metrics.body().find("luxir_control_threads 2"));
    EXPECT_NE(std::string::npos, metrics.body().find("luxir_collections{"));
    // The full scrape counts its own task in the control pool's busy gauge.
    EXPECT_TRUE(metrics.body().contains("luxir_control_busy 1\n") ||
                metrics.body().contains("luxir_control_busy 2\n"));
    auto created = request(port, http::verb::post, "/collections/_create", R"({"name":"ctl"})");
    ASSERT_EQ(200, created.result_int()) << created.body();
    auto schema = request(port, http::verb::post, "/collections/ctl/_schema",
                              R"({"fields":{"title":{"type":"text"}}})");
    ASSERT_EQ(200, schema.result_int()) << schema.body();
    auto update = request(port, http::verb::post, "/collections/ctl/_update?commit=true",
                              R"({"docs":[{"id":"1","title":"one"}]})");
    ASSERT_EQ(200, update.result_int()) << update.body();
    auto files = request(port, http::verb::get, "/collections/ctl/_stats?files=true&segments=true");
    ASSERT_EQ(200, files.result_int()) << files.body();
    EXPECT_NE(std::string::npos, files.body().find("\"live_docs\": 1"));
    auto deleted = request(port, http::verb::post, "/collections/_delete", R"({"name":"ctl"})");
    ASSERT_EQ(200, deleted.result_int()) << deleted.body();
    EXPECT_FALSE(busy.canaryRan);
  }
  server.shutdown();
}

// A delete waits out the writer's queued updates on a control thread, not a
// search worker; meanwhile the rest of the control plane keeps answering.
TEST_F(ControlPlaneTest, deleteWithPendingUpdatesHoldsNoSearchWorker) {
  std::promise<void> inCommit;
  std::promise<void> inDrain;
  std::latch unblock(1);
  std::atomic<bool> firstCommit{true}, firstDrain{true};
  LuxirNode node(twoWorkers());
  HttpServer server(node, 2, 0);
  server.start();
  auto port = server.getPort();
  for (const char* name : {R"({"name":"pending"})", R"({"name":"other"})"}) {
    ASSERT_EQ(200, request(port, http::verb::post, "/collections/_create", name).result_int());
  }
  auto* writer = node.getCollection("pending")->getShard()->requireIndexWriter().get();
  Signal::listen("initiateCommit", [&](void*, void*, void*) -> void* {
    if (firstCommit.exchange(false)) { inCommit.set_value(); unblock.wait(); }
    return nullptr;
  });
  Signal::listen("writerCloseDrain", [&, writer](void* closing, void*, void*) -> void* {
    if (closing == writer && firstDrain.exchange(false)) inDrain.set_value();
    return nullptr;
  });
  auto unlisten = scope_guard([] {
    Signal::unlisten("initiateCommit");
    Signal::unlisten("writerCloseDrain");
  });
  {
    SaturatedSearchArena busy(node);
    auto update = std::async(std::launch::async, [&] {
      return request(port, http::verb::post, "/collections/pending/_update?commit=true",
                     R"({"docs":[{"id":"1"}]})").result_int();
    });
    std::future<http::response<http::string_body>> removal;
    auto releaseCommit = scope_guard([&] { if (!unblock.try_wait()) unblock.count_down(); });
    ASSERT_EQ(std::future_status::ready, inCommit.get_future().wait_for(requestTimeout));
    removal = std::async(std::launch::async, [&] {
      return request(port, http::verb::post, "/collections/_delete", R"({"name":"pending"})");
    });
    ASSERT_EQ(std::future_status::ready, inDrain.get_future().wait_for(requestTimeout));
    auto other = request(port, http::verb::post, "/collections/other/_schema",
                         R"({"fields":{"title":{"type":"text"}}})");
    EXPECT_EQ(200, other.result_int()) << other.body();
    EXPECT_EQ(200, request(port, http::verb::get, "/_stats?files=true").result_int());
    unblock.count_down();
    auto deleted = removal.get();
    EXPECT_EQ(200, deleted.result_int()) << deleted.body();
    EXPECT_EQ(200, update.get());
    EXPECT_FALSE(busy.canaryRan);
  }
  EXPECT_THROW(node.getCollection("pending"), CollectionNotFoundError);
  server.shutdown();
}

TEST_F(ControlPlaneTest, grpcAdminAndStatsAnswerWhileSearchesHoldEveryWorker) {
  LuxirNode node(twoWorkers());
  GRPCServer server(node, 1, 0);
  std::thread serverThread([&] { server.run(); });
  auto stop = scope_guard([&] { server.shutdown(); serverThread.join(); });
  ASSERT_TRUE(server.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(server.getPort()),
                                     grpc::InsecureChannelCredentials());
  {
    SaturatedSearchArena busy(node);
    for (bool files : {false, true}) {
      api::StatsRequest stats;
      stats.files = files;
      Reply<api::StatsResponse> reply;
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + requestTimeout);
      ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::Stats, &context, stats, &reply).ok());
      EXPECT_EQ(2u, reply.msg.service->control_threads);
    }
    api::CreateCollectionRequest create;
    create.name = "grpc_ctl";
    Reply<api::CreateCollectionResponse> created;
    grpc::ClientContext creation;
    creation.set_deadline(std::chrono::system_clock::now() + requestTimeout);
    ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::CreateCollection, &creation, create, &created).ok());
    api::SchemaRequest schema;
    schema.collection = create.name;
    schema.schema.emplace();
    Reply<api::SchemaResponse> changed;
    grpc::ClientContext update;
    update.set_deadline(std::chrono::system_clock::now() + requestTimeout);
    ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::SetSchema, &update, schema, &changed).ok());
    api::DeleteCollectionRequest remove;
    remove.name = create.name;
    Reply<api::DeleteCollectionResponse> removed;
    grpc::ClientContext deletion;
    deletion.set_deadline(std::chrono::system_clock::now() + requestTimeout);
    ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::DeleteCollection, &deletion, remove, &removed).ok());
    EXPECT_FALSE(busy.canaryRan);
  }
}

}  // namespace luxir::test
