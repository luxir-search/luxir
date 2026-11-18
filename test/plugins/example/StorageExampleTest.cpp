// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <thread>

#include "CountingStorage.h"
#include "luxir/plugin/Registries.h"
#include "luxir/search/SearchRequest.h"
#include "luxir/server/HttpServer.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/util/luxir_util.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/store/ReadWait.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
using luxir::example::CountingLayer;
using luxir::example::CountingRamBackend;

class StorageExampleTest : public LuxirTest {};

namespace {

// Holds the first thread that emits `signal` once `when` holds, until the shard has been probed,
// as a read of a slow object holds a search. Destroy it only after the request
// that emits the signal has finished.
class StalledSignal {
  std::promise<void> reached, released;
  std::shared_future<void> releasedFuture = released.get_future().share();
  std::atomic<bool> first{true};

public:
  explicit StalledSignal(std::string_view signal, std::function<bool()> when = [] { return true; }) {
    Signal::listen(signal, [this, when = std::move(when)](void*, void*, void*) -> void* {
      if (when() && first.exchange(false)) {
        reached.set_value();
        releasedFuture.wait();
      }
      return nullptr;
    });
  }

  bool awaitStall() { return reached.get_future().wait_for(std::chrono::seconds(60)) == std::future_status::ready; }
  void release() { released.set_value(); }

  // While a thread is stalled, the server's only shard must still answer.
  void expectShardAnswers(int port) {
    ASSERT_TRUE(awaitStall());
    auto health = std::async(std::launch::async, [port] { return httpRequest(port, http::verb::get, "/health"); });
    bool answered = health.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    release();
    EXPECT_TRUE(answered);
    EXPECT_EQ(200, health.get().result_int());
  }
};

LuxirConfig remoteConfig(int slots = 0) {
  LuxirConfig config;
  config.store.layers = {"remote"};
  config.search.max_running_remote = slots;
  return config;
}

std::future<http::response<http::string_body>> searchAsync(int port, std::string body) {
  return std::async(std::launch::async, [port, body = std::move(body)] {
    return httpRequest(port, http::verb::post, "/collections/main/_search", body);
  });
}

// The JSON body without whitespace, as _stats pretty-prints.
std::string compact(std::string body) {
  std::erase_if(body, [](char c) { return c == ' ' || c == '\n'; });
  return body;
}

bool eventually(const std::function<bool()>& condition) {
  for (int i = 0; i < 6000 && !condition(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return condition();
}

}  // namespace

TEST_F(StorageExampleTest, remoteCollectionSearchLeavesTheShard) {
  LuxirNode node(remoteConfig());
  HttpServer server(node, 1, 0);
  server.start();
  auto update = httpRequest(server.getPort(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"a"},{"id":"b"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  for (std::string lane : {"0", "1", "-1", "-2"}) {
    SCOPED_TRACE(lane);
    StalledSignal stalled("searchRootChildStart");
    auto search = std::async(std::launch::async, [&] {
      return httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
                         R"({"query":{"all":true},"max_parallel":)" + lane + "}");
    });
    stalled.expectShardAnswers(server.getPort());
    auto response = search.get();
    EXPECT_EQ(200, response.result_int()) << response.body();
    EXPECT_NE(std::string::npos, response.body().find(R"("id":"b")")) << response.body();
  }
  server.shutdown();
}

// Production resumed after backpressure stays off the shard too. Only a
// parallel emitter emits this signal; once a batch has paused, the next one to
// emit it is a resumed one.
TEST_F(StorageExampleTest, remoteCollectionResumesProductionOffTheShard) {
  LuxirNode node(remoteConfig());
  CollectionHelper helper(node, "main");
  std::vector<Doc> docs;
  for (int i = 0; i < 2000; i++) docs.push_back(flatdoc("id", "d" + std::to_string(i), "pad_s", std::string(400, 'x')));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  HttpServer server(node, 1, 0, /*streamBufferBytes=*/4096);
  server.start();

  int64_t pausesBefore = streamPauseCount.load();
  StalledSignal stalled("emitDocsColumnsWait", [&] { return streamPauseCount.load() > pausesBefore; });
  auto search = std::async(std::launch::async, [&] {
    return httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
                       R"({"query":{"all":true},"limit":-1,"batch_size":100,"max_parallel":-1,"fields":["id","pad_s"]})");
  });
  stalled.expectShardAnswers(server.getPort());
  auto response = search.get();
  EXPECT_EQ(200, response.result_int());
  size_t count = 0;
  for (size_t pos = 0; (pos = response.body().find(R"("id":"d)", pos)) != std::string::npos; pos++) count++;
  EXPECT_EQ(2000u, count);
  server.shutdown();
}

// With one remote slot, a second search of a remote collection waits without
// starting until the first finishes, and both succeed.
TEST_F(StorageExampleTest, remoteSearchesQueueForASlot) {
  LuxirNode node(remoteConfig(1));
  HttpServer server(node, 1, 0);
  server.start();
  int port = server.getPort();
  auto update = httpRequest(port, http::verb::post, "/collections/main/_update", R"({"docs":[{"id":"a"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto& engine = node.getSearchEngine();
  {
    StalledSignal stalled("searchRootChildStart");
    auto first = searchAsync(port, R"({"query":{"all":true}})");
    ASSERT_TRUE(stalled.awaitStall());
    auto second = searchAsync(port, R"({"query":{"all":true},"max_parallel":-1})");
    EXPECT_TRUE(eventually([&] { return engine.getAdmission().snapshot().queued == 1; }));
    auto stats = engine.getAdmission().snapshot();
    EXPECT_EQ(1u, stats.running);
    EXPECT_EQ(1u, stats.admitted);
    auto nodeStats = compact(httpRequest(port, http::verb::get, "/_stats").body());
    EXPECT_NE(std::string::npos, nodeStats.find(R"("queued":1)")) << nodeStats;
    stalled.release();
    EXPECT_EQ(200, first.get().result_int());
    EXPECT_EQ(200, second.get().result_int());
  }
  auto stats = engine.getAdmission().snapshot();
  EXPECT_EQ(0u, stats.running);
  EXPECT_EQ(0u, stats.queued);
  EXPECT_EQ(2u, stats.admitted);
  EXPECT_EQ(1u, stats.waited);
  EXPECT_GT(stats.waitNanos, 0u);
  server.shutdown();
}

TEST_F(StorageExampleTest, failedRemoteSearchFreesItsSlot) {
  LuxirNode node(remoteConfig(1));
  CollectionHelper helper(node, "main");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
  HttpServer server(node, 1, 0);
  server.start();
  // Rejected while preparing, on the worker that holds the slot.
  auto failed = searchAsync(server.getPort(), R"({"query":{"all":true},"max_parallel":2})").get();
  EXPECT_EQ(400, failed.result_int()) << failed.body();
  EXPECT_EQ(0u, node.getSearchEngine().getAdmission().snapshot().running);
  auto next = searchAsync(server.getPort(), R"({"query":{"all":true}})").get();
  EXPECT_EQ(200, next.result_int()) << next.body();
  server.shutdown();
}

// A storage layer that blocks on reads records them in ReadWait: _stats sums
// them, and a profiled search reports those taken during each piece.
TEST_F(StorageExampleTest, blockingReadsShowInStatsAndProfile) {
  auto config = remoteConfig();
  config.store.options = {{"remote", {{"read_delay_us", "100"}}}};
  auto readsBefore = ReadWait::totalReads.load();
  LuxirNode node(config);
  EXPECT_GT(ReadWait::totalReads.load(), readsBefore);  // startup read its files through the layer
  CollectionHelper helper(node, "main");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "cat_s", "x")}, UpdateMessage::COMMIT).success);
  HttpServer server(node, 1, 0);
  server.start();
  auto nodeStats = compact(httpRequest(server.getPort(), http::verb::get, "/_stats").body());
  EXPECT_NE(std::string::npos, nodeStats.find(R"("read_wait":{)")) << nodeStats;

  // Segments are opened eagerly, so a read during the piece stands in for the
  // lazy fetch of a remote store.
  Signal::listen("searchProfilePieceStart", [](void*, void*, void*) -> void* {
    ReadWait::record(2000000);
    return nullptr;
  });
  auto response = searchAsync(server.getPort(),
      R"({"profile":true,"ops":{"f":{"field_facet":{"field":"cat_s"}}}})").get();
  Signal::unlisten("searchProfilePieceStart");
  EXPECT_EQ(200, response.result_int());
  EXPECT_NE(std::string::npos, response.body().find(R"("blocking_reads":1,"read_wait_us":2000)")) << response.body();
  server.shutdown();
}

TEST_F(StorageExampleTest, nodeRunsOnPluginBackendAndLayer) {
  LuxirConfig config;
  config.store.backend = "counting_ram";
  config.store.layers = {"counting"};
  config.store.checked_dir.sync = "throw";
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  auto incarnation = node.collections().catalog().current("main");
  ASSERT_TRUE(incarnation);
  auto label = "t/t0/i/" + *incarnation;
  auto* backend = node.collections().storage().find<CountingRamBackend>();
  auto* layer = node.collections().storage().find<CountingLayer>();
  ASSERT_NE(nullptr, backend);
  ASSERT_NE(nullptr, layer);
  EXPECT_GT(backend->counters->count("open", label), 0u);
  EXPECT_GT(layer->counters->count("create", label), 0u);
  EXPECT_GT(layer->counters->count("open"), 0u);
  EXPECT_NE(node.collections().storage().describe().find("counting(counting_ram)"), std::string::npos);
}

TEST_F(StorageExampleTest, unknownBackendAndLayerNamesListRegistered) {
  LuxirConfig config;
  config.store.backend = "nope";
  try {
    LuxirNode node(config);
    FAIL() << "expected an unknown backend error";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("counting_ram"), std::string::npos) << e.what();
  }
  config.store.backend = "ram";
  config.store.layers = {"nope"};
  try {
    LuxirNode node(config);
    FAIL() << "expected an unknown layer error";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("valid layers: counting"), std::string::npos) << e.what();
  }
}

TEST_F(StorageExampleTest, coreReadOnlyWrapperStaysOutsideLayers) {
  LuxirConfig config;
  ParamReader params("counting", ParamReader::Entries{});
  StorageContext ctx{config, params, {}};
  auto& storage = Registries::global().storage;
  auto layered = storage.makeLayer("counting", storage.makeBackend("counting_ram", ctx), ctx);
  std::string path = "t/t0/i/00000000001";
  ReadOnlyDirFactory readOnly(std::make_unique<CheckedDirFactory>(std::move(layered), CheckedDirMode::THROW));
  EXPECT_EQ("read_only(checked(counting(counting_ram)))", readOnly.describe());
  auto* layer = readOnly.find<CountingLayer>();
  ASSERT_NE(nullptr, layer);
  EXPECT_NE(nullptr, readOnly.find<CountingRamBackend>());
  EXPECT_EQ(&readOnly, readOnly.find<ReadOnlyDirFactory>());
  auto dir = readOnly.directory(StorageRoot::AUTHORITATIVE, path);
  EXPECT_THROW(dir->createFile("f"), ReadOnlyError);
  EXPECT_EQ(0u, layer->counters->count("create"));  // refused before the layer saw it
  EXPECT_THROW(readOnly.removeTree(StorageRoot::AUTHORITATIVE, path), ReadOnlyError);
}

TEST_F(StorageExampleTest, nodesCountIndependently) {
  LuxirConfig config;
  config.store.backend = "counting_ram";
  config.store.layers = {"counting"};
  LuxirNode first(config), second(config);
  auto* firstBackend = first.collections().storage().find<CountingRamBackend>();
  auto* secondBackend = second.collections().storage().find<CountingRamBackend>();
  auto* firstLayer = first.collections().storage().find<CountingLayer>();
  auto* secondLayer = second.collections().storage().find<CountingLayer>();
  ASSERT_TRUE(firstBackend && secondBackend && firstLayer && secondLayer);
  auto firstBefore = firstBackend->counters->count("open");
  auto before = secondBackend->counters->count("open");
  CollectionHelper h(first, "additional");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
  EXPECT_GT(firstBackend->counters->count("open"), firstBefore);
  EXPECT_EQ(before, secondBackend->counters->count("open"));
  EXPECT_GT(firstLayer->counters->count("create"), 0u);
  EXPECT_EQ(0u, secondLayer->counters->count("create"));
}

TEST_F(StorageExampleTest, unknownOptionsFailStartup) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"counting"};
  config.store.options = {{"cache", {{"bytes", "10G"}}}};
  try { LuxirNode node(config); FAIL() << "accepted an unconfigured component"; }
  catch (const ParamError& e) {
    EXPECT_STREQ("store options for unconfigured component 'cache'; configured components: ram, counting", e.what());
  }
  config.store.options = {{"counting", {{"anything", "1"}}}};
  try { LuxirNode node(config); FAIL() << "accepted an unknown layer key"; }
  catch (const ParamError& e) {
    EXPECT_STREQ("store layer 'counting': unknown parameter 'anything'; it takes no parameters", e.what());
  }
  config.store.options = {{"ram", {{"anything", "1"}}}};
  try { LuxirNode node(config); FAIL() << "accepted an unknown backend key"; }
  catch (const ParamError& e) {
    EXPECT_STREQ("store backend 'ram': unknown parameter 'anything'; it takes no parameters", e.what());
  }
}

TEST_F(StorageExampleTest, pluginRamBackendAlsoRejectsReadOnly) {
  LuxirConfig config;
  config.store.backend = "counting_ram";
  config.read_only = true;
  EXPECT_THROW(LuxirNode node(config), std::runtime_error);
}

// Reopening a writer at startup reads its commit root once and lists its
// incarnation once: the recovery check, the marker check, the writer and its
// orphan sweep share one listing. On an object store a listing is a paginated
// request priced like a write, so each extra one shows on every restart.
TEST_F(StorageExampleTest, aWriterOpenListsItsIncarnationOnce) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-open-listing-" + std::to_string(::getpid()));
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  config.store.layers = {"counting"};
  std::string label;
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
    auto incarnation = node.collections().catalog().current("main");
    ASSERT_TRUE(incarnation);
    label = "t/t0/i/" + *incarnation;
    // A marker naming a committed file: the reopen verifies it and clears it.
    auto dir = node.collections().catalog().directory(*incarnation);
    auto files = h.getIndexWriter()->snapshots.snapshot()->files;
    ASSERT_FALSE(files.empty());
    std::vector<std::string> suspects{files.front().name};
    CorruptionMarker::write(*dir, suspects, "test suspect");
  }
  LuxirNode reopened(config);
  auto* layer = reopened.collections().storage().find<CountingLayer>();
  ASSERT_NE(nullptr, layer);
  auto collection = reopened.getCollection("main");
  ASSERT_NE(nullptr, collection);
  EXPECT_EQ(2, collection->getReaderManager().getReader()->liveDocs());
  EXPECT_EQ(1u, layer->counters->count("list", label)) << "a writer open listed its incarnation more than once";
  auto dir = reopened.collections().catalog().directory(*reopened.collections().catalog().current("main"));
  EXPECT_FALSE(CorruptionMarker::present(*dir)) << "the verified marker was not cleared";
}
