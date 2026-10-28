// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/plugin/Registries.h"

using namespace luxir;

namespace {

class DummyPlugin : public Plugin {
public:
  std::string name;
  explicit DummyPlugin(std::string n) : name(std::move(n)) {}
  PluginInfo info() const override { return {name, "0", "dummy"}; }
  void registerComponents(Registries&) override {}
};

}  // namespace

TEST(PluginTest, globalRegistryListsExample) {
  initPlugins(addConfiguredPlugins);
  const Plugin* p = Registries::global().plugins.find("example");
  ASSERT_NE(p, nullptr);
  PluginInfo info = p->info();
  EXPECT_EQ(info.version, "1.0");
  EXPECT_EQ(info.description, "exercises the plugin framework");
  EXPECT_TRUE(Registries::global().plugins.frozen());
  EXPECT_EQ(Registries::global().plugins.find("missing"), nullptr);
}

TEST(PluginTest, duplicateAndInvalidNamesThrow) {
  PluginRegistry r;
  r.add(std::make_unique<DummyPlugin>("a"));
  EXPECT_THROW(r.add(std::make_unique<DummyPlugin>("a")), std::invalid_argument);
  EXPECT_THROW(r.add(std::make_unique<DummyPlugin>("Bad-Name")), std::invalid_argument);
  EXPECT_THROW(r.add(std::make_unique<DummyPlugin>("")), std::invalid_argument);
  EXPECT_EQ(r.plugins().size(), 1u);
}

TEST(PluginTest, addAfterFreezeThrows) {
  PluginRegistry r;
  r.freeze();
  EXPECT_THROW(r.add(std::make_unique<DummyPlugin>("a")), std::logic_error);
}

TEST(PluginTest, initIsIdempotent) {
  initPlugins(addConfiguredPlugins);
  size_t n = Registries::global().plugins.plugins().size();
  EXPECT_NO_THROW(initPlugins(addConfiguredPlugins));
  EXPECT_EQ(Registries::global().plugins.plugins().size(), n);
}

TEST(PluginTest, paramReaderUsesNeutralErrors) {
  ParamReader params("component", ParamReader::Entries{});
  EXPECT_EQ(7, params.getInt("count", 7));
  try {
    params.requireInt("count");
    FAIL();
  } catch (const std::invalid_argument& e) {
    EXPECT_STREQ("component: 'count' is required", e.what());
  }
}
