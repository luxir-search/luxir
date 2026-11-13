// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>

#include "CountingStorage.h"
#include "luxir/plugin/Registries.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/util/luxir_util.h"
#include "luxir/store/CheckedDirFactory.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
using luxir::example::CountingLayer;
using luxir::example::CountingRamBackend;

class StorageExampleTest : public LuxirTest {};

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
