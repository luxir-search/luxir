// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <memory_resource>
#include <stdexcept>
#include <string>

#include "luxir/collation/CollationRegistry.h"
#include "luxir/plugin/Registries.h"

using namespace luxir;

namespace {

// Keys are the value with its bytes reversed, whatever that orders.
struct ReverseCollator : Collator {
  void appendKey(std::string_view value, std::string& key) override { key.append(value.rbegin(), value.rend()); }
};

CollationRegistry::Maker reverseMaker() {
  return [](const api::AnalyzerComponent& def) -> std::unique_ptr<const CollatorFactory> {
    if (!def.params.empty()) throw std::invalid_argument("collation 'reverse' takes no parameters");
    struct Factory : CollatorFactory {
      std::unique_ptr<Collator> create() const override { return std::make_unique<ReverseCollator>(); }
    };
    return std::make_unique<Factory>();
  };
}

api::AnalyzerComponent component(std::string_view name) {
  api::AnalyzerComponent c;
  c.name = name;
  return c;
}

template <class F>
std::string messageOf(F&& fn) {
  try { fn(); } catch (const std::exception& e) { return e.what(); }
  return {};
}

}  // namespace

TEST(CollationRegistryTest, makesFactoriesByName) {
  CollationRegistry registry;
  registry.add("reverse", reverseMaker());
  auto made = registry.make(component("reverse"));
  EXPECT_EQ("reverse", made->name);
  EXPECT_EQ(R"("reverse")", made->canonical);
  std::string key = "k:";
  made->create()->appendKey("abc", key);
  EXPECT_EQ("k:cba", key);
  EXPECT_NE(made->create().get(), made->create().get());  // one collator per use
}

TEST(CollationRegistryTest, unknownAndUnnamedListRegisteredNames) {
  CollationRegistry registry;
  EXPECT_EQ("unknown collation 'x'; valid collations: none", messageOf([&] { registry.make(component("x")); }));
  EXPECT_EQ("collation has no name; valid collations: none", messageOf([&] { registry.make(component("")); }));
  registry.add("zeta", reverseMaker());
  registry.add("alpha", reverseMaker());
  EXPECT_EQ("unknown collation 'x'; valid collations: alpha, zeta", messageOf([&] { registry.make(component("x")); }));
  EXPECT_EQ((std::vector<std::string>{"alpha", "zeta"}), registry.names());
}

TEST(CollationRegistryTest, makerErrorsReachTheCaller) {
  CollationRegistry registry;
  registry.add("reverse", reverseMaker());
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerComponent withParams;
  ASSERT_TRUE(api::read_json(withParams, R"({"name":"reverse","params":{"locale":"fr"}})", arena));
  EXPECT_EQ("collation 'reverse' takes no parameters", messageOf([&] { registry.make(withParams); }));
  registry.add("empty", [](const api::AnalyzerComponent&) -> std::unique_ptr<const CollatorFactory> { return nullptr; });
  EXPECT_NE(std::string::npos, messageOf([&] { registry.make(component("empty")); }).find("made no factory"));
}

TEST(CollationRegistryTest, duplicatesAndBadNamesThrow) {
  CollationRegistry registry;
  registry.add("reverse", reverseMaker());
  EXPECT_THROW(registry.add("reverse", reverseMaker()), std::invalid_argument);
  for (const char* bad : {"", "Mixed", "has-dash", "sp ace"}) {
    EXPECT_THROW(registry.add(bad, reverseMaker()), std::invalid_argument) << bad;
  }
  EXPECT_THROW(registry.add("no_maker", nullptr), std::invalid_argument);
  EXPECT_EQ((std::vector<std::string>{"reverse"}), registry.names());
}

TEST(CollationRegistryTest, frozenRegistryRejectsAdds) {
  CollationRegistry registry;
  registry.add("reverse", reverseMaker());
  registry.freeze();
  EXPECT_TRUE(registry.frozen());
  EXPECT_THROW(registry.add("late", reverseMaker()), std::logic_error);
  EXPECT_NO_THROW(registry.make(component("reverse")));  // lookups keep working
}

TEST(CollationRegistryTest, globalRegistryIsFrozenAfterInit) {
  initPlugins(addConfiguredPlugins);
  EXPECT_TRUE(Registries::global().collation.frozen());
  EXPECT_THROW(Registries::global().collation.add("late", reverseMaker()), std::logic_error);
}
