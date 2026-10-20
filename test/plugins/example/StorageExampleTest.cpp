// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "CountingStorage.h"
#include "luxir/plugin/Registries.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
using luxir::example::CountingStorage;

class StorageExampleTest : public LuxirTest {
protected:
  void SetUp() override {
    LuxirTest::SetUp();
    CountingStorage::reset();
  }
};

TEST_F(StorageExampleTest, nodeRunsOnPluginBackendAndLayer) {
  LuxirConfig config;
  config.store.backend = "counting_ram";
  config.store.layers = {"counting"};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  auto label = CollectionId::of("main").label();
  EXPECT_GT(CountingStorage::count("backend", "create", label), 0u);
  EXPECT_GT(CountingStorage::count("layer", "create", label), 0u);
  EXPECT_GT(CountingStorage::count("layer", "open"), 0u);
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
  StorageContext ctx{config, config.store.options, {}};
  auto& storage = Registries::global().storage;
  auto layered = storage.makeLayer("counting", storage.makeBackend("counting_ram", ctx), ctx);
  auto id = CollectionId::of("ro");
  layered->incarnation(id, "i1", true);
  ReadOnlyDirFactory readOnly(std::move(layered));
  EXPECT_EQ("read_only(counting(counting_ram))", readOnly.describe());
  auto dir = readOnly.incarnation(id, "i1", false);
  EXPECT_THROW(dir->createFile("f"), ReadOnlyError);
  EXPECT_EQ(0u, CountingStorage::count("layer", "create"));  // refused before the layer saw it
  EXPECT_THROW(readOnly.incarnation(id, "i2", true), ReadOnlyError);
}
