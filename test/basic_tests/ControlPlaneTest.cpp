// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
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

class ControlPlaneTest : public LuxirTest {};

// Holds every search-arena worker, as long searches would. A canary queued
// behind them proves nothing else ran there meanwhile.
class SaturatedSearchArena {
  LuxirNode& node;
  std::atomic<int> started{0};
  std::atomic<bool> released{false};
  std::latch finished;

public:
  std::atomic<bool> canaryRan{false};

  explicit SaturatedSearchArena(LuxirNode& node) : node(node), finished(node.getWorkers() + 1) {
    for (int i = 0; i < node.getWorkers(); i++) {
      node.getTaskArena().enqueue([this] {
        started++;
        released.wait(false);
        finished.count_down();
      });
    }
    while (started.load() < node.getWorkers()) std::this_thread::yield();
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
    EXPECT_EQ(200, httpRequest(port, http::verb::get, "/_stats").result_int());
    auto metrics = httpRequest(port, http::verb::get, "/metrics");
    ASSERT_EQ(200, metrics.result_int());
    EXPECT_NE(std::string::npos, metrics.body().find("luxir_control_threads 2"));
    auto created = httpRequest(port, http::verb::post, "/collections/_create", R"({"name":"ctl"})");
    ASSERT_EQ(200, created.result_int()) << created.body();
    auto schema = httpRequest(port, http::verb::post, "/collections/ctl/_schema",
                              R"({"fields":{"title":{"type":"text"}}})");
    ASSERT_EQ(200, schema.result_int()) << schema.body();
    auto update = httpRequest(port, http::verb::post, "/collections/ctl/_update?commit=true",
                              R"({"docs":[{"id":"1","title":"one"}]})");
    ASSERT_EQ(200, update.result_int()) << update.body();
    auto files = httpRequest(port, http::verb::get, "/collections/ctl/_stats?files=true&segments=true");
    ASSERT_EQ(200, files.result_int()) << files.body();
    EXPECT_NE(std::string::npos, files.body().find("\"live_docs\": 1"));
    auto deleted = httpRequest(port, http::verb::post, "/collections/_delete", R"({"name":"ctl"})");
    ASSERT_EQ(200, deleted.result_int()) << deleted.body();
    EXPECT_FALSE(busy.canaryRan);
  }
  server.shutdown();
}

// A delete waits out the writer's queued updates on a control thread, not a
// search worker; meanwhile the rest of the control plane keeps answering.
TEST_F(ControlPlaneTest, deleteWithPendingUpdatesHoldsNoSearchWorker) {
  LuxirNode node(twoWorkers());
  HttpServer server(node, 2, 0);
  server.start();
  auto port = server.getPort();
  for (const char* name : {R"({"name":"pending"})", R"({"name":"other"})"}) {
    ASSERT_EQ(200, httpRequest(port, http::verb::post, "/collections/_create", name).result_int());
  }

  std::atomic<bool> armed{true};
  std::latch inCommit(1);
  std::promise<void> unblock;
  auto unblocked = unblock.get_future().share();
  Signal::listen("initiateCommit", [&](void*, void*, void*) -> void* {
    if (armed.exchange(false)) { inCommit.count_down(); unblocked.wait(); }
    return nullptr;
  });
  {
    SaturatedSearchArena busy(node);
    auto update = std::async(std::launch::async, [&] {
      return httpRequest(port, http::verb::post, "/collections/pending/_update?commit=true",
                         R"({"docs":[{"id":"1"}]})").result_int();
    });
    inCommit.wait();
    auto removal = std::async(std::launch::async, [&] {
      return httpRequest(port, http::verb::post, "/collections/_delete", R"({"name":"pending"})");
    });
    while (node.getControlExecutor().stats().busy == 0) std::this_thread::yield();
    // The delete occupies one control thread (and its tenant's lifecycle
    // lock); the other thread still serves schema changes and stats.
    auto other = httpRequest(port, http::verb::post, "/collections/other/_schema",
                             R"({"fields":{"title":{"type":"text"}}})");
    EXPECT_EQ(200, other.result_int()) << other.body();
    EXPECT_EQ(200, httpRequest(port, http::verb::get, "/_stats?files=true").result_int());
    unblock.set_value();
    auto deleted = removal.get();
    EXPECT_EQ(200, deleted.result_int()) << deleted.body();
    update.get();  // answered, whether its commit won the race with the close
    EXPECT_FALSE(busy.canaryRan);
  }
  Signal::unlisten("initiateCommit");
  EXPECT_THROW(node.getCollection("pending"), CollectionNotFoundError);
  server.shutdown();
}

TEST_F(ControlPlaneTest, grpcAdminAndStatsAnswerWhileSearchesHoldEveryWorker) {
  LuxirNode node(twoWorkers());
  GRPCServer server(node, 1, 0);
  std::thread serverThread([&] { server.run(); });
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
      ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::Stats, &context, stats, &reply).ok());
      EXPECT_EQ(2u, reply.msg.service->control_threads);
    }
    api::CreateCollectionRequest create;
    create.name = "grpc_ctl";
    Reply<api::CreateCollectionResponse> created;
    grpc::ClientContext creation;
    ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::CreateCollection, &creation, create, &created).ok());
    api::SchemaRequest schema;
    schema.collection = create.name;
    schema.schema.emplace();
    Reply<api::SchemaResponse> changed;
    grpc::ClientContext update;
    ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::SetSchema, &update, schema, &changed).ok());
    api::DeleteCollectionRequest remove;
    remove.name = create.name;
    Reply<api::DeleteCollectionResponse> removed;
    grpc::ClientContext deletion;
    ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::DeleteCollection, &deletion, remove, &removed).ok());
    EXPECT_FALSE(busy.canaryRan);
  }
  server.shutdown();
  serverThread.join();
}

}  // namespace luxir::test
