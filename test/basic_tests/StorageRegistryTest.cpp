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
const ParamReader noParams("store backend 'ram'", ParamReader::Entries{});
StorageContext context() { return {config, noParams, {}}; }

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
  app.parse("--store.layer a --store.layer b --store.option a.k=v --store.option b.j=w --store.option a.k=last");
  EXPECT_EQ(parsed.store.layers, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(parsed.store.options, (decltype(parsed.store.options){{"a", {{"k", "last"}}}, {"b", {{"j", "w"}}}}));
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
  EXPECT_NE(messageOf([&] { registry.makeBackend("s3", context()); }).find("valid backends: fs, ram"), std::string::npos);
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

TEST(StorageRegistryTest, layersWrapInOrder) {
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

}

TEST(StorageRegistryTest, malformedOptionsFailAtParseTime) {
  for (auto option : {"key=value", ".key=value", "ram.=value", "ram.key", "Ram.key=value", "a-b.key=value"}) {
    LuxirConfig parsed;
    CLI::App app;
    parsed.addOptions(app);
    EXPECT_THROW(app.parse("--store.option " + std::string(option)), CLI::ValidationError) << option;
  }
}

TEST(StorageRegistryTest, optionsAreTypedAndScoped) {
  LuxirConfig config;
  CLI::App app;
  config.addOptions(app);
  const char* argv[] = {"luxir", "--store.option=s3.bucket=logs", "--store.option=s3.numeric=\"123\"",
                        "--store.option=s3.count=123", "--store.option=s3.ratio=1.5", "--store.option=s3.enabled=true",
                        "--store.option=s3.disabled=false", "--store.option=s3.bytes=10G", "--store.option=cache.bytes=1048576"};
  app.parse((int)std::size(argv), argv);
  StorageOptions options(config.store);
  auto params = options.reader("s3", "store backend 's3'");
  EXPECT_EQ("logs", params.getString("bucket"));
  EXPECT_EQ("123", params.getString("numeric"));
  EXPECT_EQ(123, params.requireInt("count"));
  EXPECT_EQ(1.5, params.getDouble("ratio", 0));
  EXPECT_TRUE(params.getBool("enabled", false));
  EXPECT_FALSE(params.getBool("disabled", true));
  EXPECT_EQ("10G", params.getString("bytes"));
  EXPECT_NO_THROW(params.rejectUnknown());
  auto cache = options.reader("cache", "store layer 'cache'");
  EXPECT_EQ(1048576, cache.requireInt("bytes"));
  EXPECT_NO_THROW(cache.rejectUnknown());
}

TEST(StorageRegistryTest, nonScalarAndMalformedValuesStayStrings) {
  for (std::string text : {"null", "[]", "{}", "", "1x", "truex", "01", "\"unclosed"}) {
    StoreConfig config;
    config.options = {{"s3", {{"bucket", text}}}};
    StorageOptions options(config);
    auto params = options.reader("s3", "store backend 's3'");
    EXPECT_EQ(text, params.getString("bucket"));
  }
}

TEST(StorageRegistryTest, unknownKeyListsAcceptedNames) {
  StoreConfig config;
  config.options = {{"cache", {{"byets", "1G"}}}};
  StorageOptions options(config);
  auto params = options.reader("cache", "store layer 'cache'");
  EXPECT_EQ(1024u, params.getBytes("bytes", 1024));
  std::string key = "path";
  EXPECT_EQ("", params.getString(key));
  key.assign("gone"); // validation runs after the maker's temporary key is gone
  EXPECT_EQ("store layer 'cache': unknown parameter 'byets'; valid: bytes, path",
            messageOf([&] { params.rejectUnknown(); }));
}

TEST(StorageRegistryTest, byteSizesUseCliUnits) {
  StoreConfig config;
  config.options = {{"cache", {{"integer", "1048576"}, {"size", "2 kb"}, {"large", "1TiB"},
                               {"negative", "-1"}, {"string_negative", R"("-1")"}, {"fraction", "1.5"},
                               {"bad", "3Q"}, {"overflow", "18446744073709551616"}}}};
  StorageOptions options(config);
  auto params = options.reader("cache", "store layer 'cache'");
  EXPECT_EQ(1048576u, params.getBytes("integer", 0));
  EXPECT_EQ(2048u, params.getBytes("size", 0));
  EXPECT_EQ(1ULL << 40, params.getBytes("large", 0));
  for (auto key : {"negative", "string_negative", "fraction", "bad", "overflow"}) {
    auto error = messageOf([&] { params.getBytes(key, 0); });
    EXPECT_TRUE(error.starts_with("store layer 'cache': '" + std::string(key) + "'")) << error;
  }
}

TEST(StorageRegistryTest, ramMakerRejectsReadOnly) {
  LuxirConfig config;
  config.read_only = true;
  EXPECT_NO_THROW(config.normalize());
  StorageContext ctx{config, noParams, {}};
  EXPECT_EQ("--read-only requires a persistent --store.backend such as fs; there is no existing data to serve with backend=ram",
            messageOf([&] { StorageRegistry::global().makeBackend("ram", ctx); }));
}
