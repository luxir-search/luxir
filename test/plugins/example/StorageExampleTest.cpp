// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>

#include "CountingStorage.h"
#include "luxir/plugin/Registries.h"
#include "luxir/search/SearchRequest.h"
#include "luxir/server/HttpServer.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/CheckedDirFactory.h"
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

  // While a thread is stalled, the server's only shard must still answer.
  void expectShardAnswers(int port) {
    ASSERT_EQ(std::future_status::ready, reached.get_future().wait_for(std::chrono::seconds(60)));
    auto health = std::async(std::launch::async, [port] { return httpRequest(port, http::verb::get, "/health"); });
    bool answered = health.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    released.set_value();
    EXPECT_TRUE(answered);
    EXPECT_EQ(200, health.get().result_int());
  }
};

LuxirConfig remoteConfig() {
  LuxirConfig config;
  config.store.layers = {"remote"};
  return config;
}

}  // namespace

TEST_F(StorageExampleTest, remoteCollectionSearchLeavesTheShard) {
  LuxirNode node(remoteConfig());
  HttpServer server(node, 1, 0);
  server.start();
  auto update = httpRequest(server.getPort(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"a"},{"id":"b"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  for (std::string lane : {"0", "1", "-1"}) {
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
