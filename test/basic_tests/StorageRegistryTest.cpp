// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <CLI/CLI.hpp>
#include <gtest/gtest.h>

#include "luxir/plugin/Registries.h"
#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/StorageRegistry.h"

using namespace luxir;

namespace {

LuxirConfig config;
StorageContext context() { return {config, config.store.options, {}}; }

std::unique_ptr<DirectoryFactory> ramMaker(const StorageContext&) { return std::make_unique<RAMDirFactory>(); }

template <class F>
std::string messageOf(F&& fn) {
  try { fn(); } catch (const std::exception& e) { return e.what(); }
  return {};
}

}  // namespace

TEST(StorageRegistryTest, repeatedLayerAndOptionFlagsParse) {
  LuxirConfig parsed;
  CLI::App app;
  parsed.addOptions(app);
  app.parse("--store.layer a --store.layer b --store.option k=v --store.option j=w");
  EXPECT_EQ(parsed.store.layers, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(parsed.store.options, (std::map<std::string, std::string>{{"j", "w"}, {"k", "v"}}));
}

TEST(StorageRegistryTest, builtinsAreRegistered) {
  initPlugins(addConfiguredPlugins);
  auto& storage = Registries::global().storage;
  EXPECT_TRUE(storage.frozen());
  auto names = storage.backendNames();
  EXPECT_NE(std::ranges::find(names, std::string("ram")), names.end());
  EXPECT_NE(std::ranges::find(names, std::string("fs")), names.end());
  EXPECT_TRUE(std::ranges::is_sorted(names));
  auto ram = storage.makeBackend("ram", context());
  EXPECT_EQ("ram", ram->describe());
}

TEST(StorageRegistryTest, unknownNamesListRegisteredOnes) {
  StorageRegistry registry;
  registerBuiltinStorage(registry);
  registry.addLayer("zlayer", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) { return inner; });
  registry.addLayer("alayer", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) { return inner; });
  EXPECT_NE(messageOf([&] { registry.makeBackend("s3", context()); }).find("valid backends: fs, ram, ram_object"), std::string::npos);
  auto layer = messageOf([&] { registry.makeLayer("nope", std::make_unique<RAMDirFactory>(), context()); });
  EXPECT_NE(layer.find("'nope'"), std::string::npos);
  EXPECT_NE(layer.find("valid layers: alayer, zlayer"), std::string::npos);
  StorageRegistry empty;
  EXPECT_NE(messageOf([&] { empty.makeLayer("x", nullptr, context()); }).find("none registered"), std::string::npos);
}

TEST(StorageRegistryTest, duplicatesAndBadNamesThrow) {
  StorageRegistry registry;
  registry.addBackend("a", ramMaker);
  EXPECT_THROW(registry.addBackend("a", ramMaker), std::invalid_argument);
  EXPECT_THROW(registry.addBackend("Bad-Name", ramMaker), std::invalid_argument);
  EXPECT_THROW(registry.addBackend("", ramMaker), std::invalid_argument);
  EXPECT_THROW(registry.addBackend("b", nullptr), std::invalid_argument);
  auto layer = [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) { return inner; };
  registry.addLayer("a", layer);  // layers and backends are separate namespaces
  EXPECT_THROW(registry.addLayer("a", layer), std::invalid_argument);
}

TEST(StorageRegistryTest, frozenRegistryRejectsRegistration) {
  StorageRegistry registry;
  registry.freeze();
  EXPECT_THROW(registry.addBackend("a", ramMaker), std::logic_error);
  EXPECT_THROW(registry.addLayer("a", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) { return inner; }),
               std::logic_error);
}

TEST(StorageRegistryTest, layersWrapInOrderAndOptionsParse) {
  StorageRegistry registry;
  registry.addBackend("base", ramMaker);
  struct Tagged : DelegatingDirFactory {
    std::string tag;
    Tagged(std::unique_ptr<DirectoryFactory> inner, std::string tag) : DelegatingDirFactory(std::move(inner)), tag(std::move(tag)) {}
    std::string describe() const override { return tag + "(" + delegate->describe() + ")"; }
  };
  for (std::string name : {"one", "two"}) {
    registry.addLayer(name, [name](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) {
      return std::make_unique<Tagged>(std::move(inner), name);
    });
  }
  auto ctx = context();
  auto factory = registry.makeLayer("two", registry.makeLayer("one", registry.makeBackend("base", ctx), ctx), ctx);
  EXPECT_EQ("two(one(ram))", factory->describe());

  LuxirConfig withOptions;
  withOptions.store.options = {{"size", "64K"}, {"big", "2G"}, {"plain", "10"}, {"bad", "x"}, {"unit", "3Q"}};
  StorageContext ctx2{withOptions, withOptions.store.options, {}};
  EXPECT_EQ(64u * 1024, ctx2.optionBytes("size", 0));
  EXPECT_EQ(2ULL << 30, ctx2.optionBytes("big", 0));
  EXPECT_EQ(10u, ctx2.optionBytes("plain", 0));
  EXPECT_EQ(7u, ctx2.optionBytes("missing", 7));
  EXPECT_THROW(ctx2.optionBytes("bad", 0), std::invalid_argument);
  EXPECT_THROW(ctx2.optionBytes("unit", 0), std::invalid_argument);
  EXPECT_EQ("64K", ctx2.option("size"));
  EXPECT_FALSE(ctx2.option("missing"));
  EXPECT_EQ("d", ctx2.optionOr("missing", "d"));
}
