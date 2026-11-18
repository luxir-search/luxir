// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <csignal>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/server/HttpServer.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/server/Prometheus.h"
#include "luxir/server/Stats.h"
#include "luxir/util/InflightRegistry.h"
#include "luxir/util/SignalWriter.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"

namespace luxir::test {

namespace {

using Request = InflightRegistry::Request;

struct DataDir {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("luxir_inflight_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  DataDir() { std::filesystem::create_directories(path); }
  ~DataDir() { std::filesystem::remove_all(path); }
  std::string str() const { return path.string(); }
};

LuxirConfig fsConfig(const DataDir& data) {
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = data.str();
  config.normalize();
  return config;
}

Request search(std::string_view collection, std::string_view id = "r1") {
  return {.tenant = "t0", .collection = collection, .requestId = id, .operation = "q:top_docs", .maxParallel = -1};
}

std::string dumpText(const InflightRegistry& registry, uint64_t crashingThread) {
  FILE* file = tmpfile();
  {
    SignalWriter out(fileno(file));
    registry.dump(out, crashingThread);
  }
  rewind(file);
  std::string text;
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), file)) text.append(buf, n);
  fclose(file);
  return text;
}

} // namespace

class InflightRegistryTest : public LuxirTest {};

TEST_F(InflightRegistryTest, claimsAndReleasesWithoutAllocating) {
  InflightRegistry registry;
  InflightRegistry::Entry first, second;
  {
    memtrack::AllocScope scope;
    first = registry.track(search("products"));
    second = registry.track({.tenant = "t1", .collection = "logs", .kind = InflightKind::UPDATE,
                             .transport = InflightTransport::HTTP, .phase = InflightPhase::QUEUED});
    second.phase(InflightPhase::RUNNING);
    long bytes = scope.bytes();
    EXPECT_EQ(0, bytes);
  }
  auto running = registry.running();
  ASSERT_EQ(2u, running.size());
  EXPECT_EQ("products", running[0].collection);
  EXPECT_EQ("r1", running[0].requestId);
  EXPECT_EQ(-1, running[0].maxParallel);
  EXPECT_EQ(InflightRegistry::currentThread(), running[0].thread);
  EXPECT_EQ(InflightKind::UPDATE, running[1].kind);
  EXPECT_EQ(InflightPhase::RUNNING, running[1].phase);

  auto moved = std::move(first);
  EXPECT_FALSE(first.tracked());
  EXPECT_EQ(2u, registry.running().size());
  moved.release();
  second = {};
  EXPECT_TRUE(registry.running().empty());
}

TEST_F(InflightRegistryTest, overflowIsCountedAndLongFieldsTruncate) {
  InflightRegistry registry;
  std::vector<InflightRegistry::Entry> entries;
  for (uint32_t i = 0; i < InflightRegistry::kSlots; i++) entries.push_back(registry.track(search("c")));
  auto extra = registry.track(search(std::string(200, 'x')));
  EXPECT_FALSE(extra.tracked());
  EXPECT_EQ(1u, registry.untracked());
  extra.release();
  EXPECT_EQ(0u, registry.untracked());

  entries.pop_back();
  auto again = registry.track(search(std::string(200, 'x')));
  ASSERT_TRUE(again.tracked());
  auto running = registry.running();
  ASSERT_EQ(InflightRegistry::kSlots, running.size());
  auto truncated = std::ranges::find_if(running, [](const auto& r) { return r.collection != "c"; });
  ASSERT_NE(running.end(), truncated);
  EXPECT_EQ(std::string(InflightRegistry::kCollectionBytes - 1, 'x'), truncated->collection);
}

// The crash handler prints this from a signal handler; call it directly.
TEST_F(InflightRegistryTest, dumpFormatsRunningRequests) {
  InflightRegistry registry;
  EXPECT_EQ("Running requests: 0\n", dumpText(registry, 0));
  auto mine = registry.track({.tenant = "t0", .collection = "products", .requestId = "abc", .operation = "q:top_docs",
                              .maxParallel = -1, .transport = InflightTransport::GRPC});
  auto text = dumpText(registry, InflightRegistry::currentThread());
  EXPECT_TRUE(text.starts_with("Running requests: 1\n")) << text;
  EXPECT_NE(std::string::npos, text.find("  search grpc running tenant=t0 collection=products request_id=abc "
                                         "max_parallel=-1 age_ms=")) << text;
  EXPECT_NE(std::string::npos, text.find(" op=q:top_docs [crashing thread]\n")) << text;
  EXPECT_EQ(std::string::npos, dumpText(registry, 1).find("crashing")) << text;
}

// A node killed without a chance to shut down (SIGKILL, the OOM killer) leaves
// its request table behind; the next node on the directory reports it.
TEST_F(InflightRegistryTest, uncleanExitIsReportedAtTheNextStart) {
  DataDir data;
  {
    LuxirNode node(fsConfig(data));
    auto running = node.inflight().track(search("products"));
    auto other = node.inflight().track({.tenant = "t0", .collection = "logs", .operation = "docs=3 deletes=0",
                                        .kind = InflightKind::UPDATE, .transport = InflightTransport::HTTP});
    auto done = node.inflight().track(search("finished"));
    done.release();
    EXPECT_FALSE(node.inflight().previousExit());
    node.inflight().abandonForTests();
  }
  {
    LuxirNode node(fsConfig(data));
    const auto& exit = node.inflight().previousExit();
    ASSERT_TRUE(exit);
    EXPECT_FALSE(exit->cause.empty());
    EXPECT_GT(exit->lastSampleMs, 0);
    ASSERT_EQ(2u, exit->requests.size());
    EXPECT_EQ("products", exit->requests[0].collection);
    EXPECT_EQ("logs", exit->requests[1].collection);
    EXPECT_NE(std::string::npos, exit->describe().find("collections: t0/logs (1) t0/products (1)")) << exit->describe();

    std::pmr::monotonic_buffer_resource resource;
    api::StatsResponse stats;
    gatherStats(node, {}, stats, resource);
    ASSERT_TRUE(stats.unclean_exit);
    EXPECT_EQ(exit->cause, stats.unclean_exit->cause);
    ASSERT_EQ(2u, stats.unclean_exit->requests.size());
    EXPECT_EQ("update", stats.unclean_exit->requests[1].kind);
    auto text = renderPrometheus(stats);
    EXPECT_NE(std::string::npos, text.find("luxir_unclean_exit_requests 2\n")) << text;
    EXPECT_NE(std::string::npos,
              text.find("luxir_unclean_exit_collection_requests{tenant=\"t0\",collection=\"products\"} 1\n")) << text;
  }
  // That node shut down cleanly: nothing to report any more.
  LuxirNode node(fsConfig(data));
  EXPECT_FALSE(node.inflight().previousExit());
  std::pmr::monotonic_buffer_resource resource;
  api::StatsResponse stats;
  gatherStats(node, {}, stats, resource);
  EXPECT_FALSE(stats.unclean_exit);
}

TEST_F(InflightRegistryTest, crashHandlerRecordsTheCause) {
  DataDir data;
  {
    InflightRegistry registry({.directory = data.str()});
    auto running = registry.track(search("products"));
    InflightRegistry::recordFatalSignal(SIGSEGV);
    registry.abandonForTests();
  }
  {
    InflightRegistry registry({.directory = data.str()});
    ASSERT_TRUE(registry.previousExit());
    EXPECT_EQ("signal", registry.previousExit()->cause);
    EXPECT_EQ("SIGSEGV", registry.previousExit()->detail);
    InflightRegistry::recordTerminate("uncaught exception std::runtime_error: boom");
    registry.abandonForTests();
  }
  InflightRegistry registry({.directory = data.str()});
  ASSERT_TRUE(registry.previousExit());
  EXPECT_EQ("uncaught_exception", registry.previousExit()->cause);
  EXPECT_EQ("uncaught exception std::runtime_error: boom", registry.previousExit()->detail);
  EXPECT_TRUE(registry.previousExit()->requests.empty());
}

TEST_F(InflightRegistryTest, corruptRecordIsIgnored) {
  DataDir data;
  auto file = data.path / InflightRegistry::kFileName;
  std::ofstream(file) << "not a request table";
  EXPECT_FALSE(InflightRegistry({.directory = data.str()}).previousExit());

  // A valid record with one damaged slot is ignored as a whole.
  {
    InflightRegistry registry({.directory = data.str()});
    auto running = registry.track(search("products"));
    registry.abandonForTests();
  }
  auto bytes = std::filesystem::file_size(file);
  std::string image(bytes, '\0');
  std::fstream(file, std::ios::in | std::ios::binary).read(image.data(), (std::streamsize)bytes);
  for (size_t at = bytes - InflightRegistry::kSlots * 256; at < bytes; at += 256) image[at + 28] = (char)0x7f;  // kind
  std::fstream(file, std::ios::out | std::ios::in | std::ios::binary).write(image.data(), (std::streamsize)bytes);
  InflightRegistry registry({.directory = data.str()});
  EXPECT_FALSE(registry.previousExit());
  EXPECT_TRUE(registry.running().empty());
}

// Requests through the transports release their slots when they finish.
TEST_F(InflightRegistryTest, httpRequestsReleaseTheirSlots) {
  LuxirNode node;
  HttpServer server(node, 2, 0);
  server.start();
  auto update = httpRequest(server.getPort(), http::verb::post, "/collections/main/_update",
                            R"({"docs":[{"id":"r1","title_w":"token"}],"commit":{}})");
  EXPECT_EQ(200, update.result_int()) << update.body();
  auto found = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
                           R"({"query":"title_w:token","fields":["id"]})");
  EXPECT_EQ(200, found.result_int()) << found.body();
  auto stats = httpRequest(server.getPort(), http::verb::get, "/_stats");
  EXPECT_EQ(200, stats.result_int());
  // The request object outlives the response write by a moment.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!node.inflight().running().empty() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(node.inflight().running().empty());
  EXPECT_EQ(0u, node.inflight().untracked());
  server.shutdown();
}

} // namespace luxir::test
