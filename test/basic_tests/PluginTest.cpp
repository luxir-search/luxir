// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/plugin/Registries.h"
#include "luxir/plugin/PluginInit.h"
#include "luxir/server/LuxirNode.h"

using namespace luxir;

namespace {

class DummyPlugin : public Plugin {
public:
  std::string name;
  explicit DummyPlugin(std::string n) : name(std::move(n)) {}
  PluginInfo info() const override { return {name, "0", "dummy"}; }
  void registerComponents(Registries&) override {}
};

class LifecyclePlugin : public DummyPlugin {
  std::vector<std::string>& calls;
  bool fail;

public:
  LifecyclePlugin(std::string name, std::vector<std::string>& calls, bool fail = false)
    : DummyPlugin(std::move(name)), calls(calls), fail(fail) {}
  std::unique_ptr<PluginNodeState> createNodeState(LuxirNode&) override {
    class State final : public PluginNodeState {
      std::vector<std::string>& calls;
      std::string name;
    public:
      State(std::vector<std::string>& calls, std::string name) : calls(calls), name(std::move(name)) {}
      ~State() noexcept override { calls.push_back("stop:" + name); }
    };
    calls.push_back("start:" + name);
    if (fail) throw std::runtime_error(name);
    return std::make_unique<State>(calls, name);
  }
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

TEST(PluginTest, differentAdderIsRejected) {
  initPlugins(addConfiguredPlugins);
  try {
    initPlugins(nullptr);
    FAIL();
  } catch (const std::logic_error& e) {
    EXPECT_STREQ("initPlugins called with a different plugin adder", e.what());
  }
  EXPECT_NO_THROW(initPlugins(addConfiguredPlugins));
}

TEST(PluginTest, nullAdderIsIdempotentAndDistinctFromConfigured) {
  int attempts = 0;
  auto initialize = [&](auto adder) {
    attempts++;
    PluginRegistry plugins;
    adder(plugins);
  };
  detail::PluginInit empty;
  EXPECT_NO_THROW(empty.run(nullptr, initialize));
  EXPECT_NO_THROW(empty.run(nullptr, initialize));
  EXPECT_THROW(empty.run(addConfiguredPlugins, initialize), std::logic_error);
  EXPECT_EQ(1, attempts);

  detail::PluginInit configured;
  EXPECT_NO_THROW(configured.run(addConfiguredPlugins, initialize));
  EXPECT_NO_THROW(configured.run(addConfiguredPlugins, initialize));
  EXPECT_THROW(configured.run(nullptr, initialize), std::logic_error);
  EXPECT_EQ(2, attempts);
}

TEST(PluginTest, initializationFailureIsTerminal) {
  detail::PluginInit state;
  PluginRegistry plugins;
  int attempts = 0;
  auto initialize = [&](auto adder) {
    attempts++;
    adder(plugins);
    plugins.freeze();
    throw std::runtime_error("registration failed after freeze");
  };
  void (*adders[])(PluginRegistry&) = {nullptr, nullptr, addConfiguredPlugins};
  for (auto adder : adders) {
    try {
      state.run(adder, initialize);
      FAIL();
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ("registration failed after freeze", e.what());
    }
  }
  EXPECT_EQ(1, attempts);
}

TEST(PluginTest, failedNodeStateCreationUnwindsCompletedStates) {
  LuxirNode node;
  std::vector<std::string> calls;
  PluginRegistry plugins;
  plugins.add(std::make_unique<LifecyclePlugin>("a", calls));
  plugins.add(std::make_unique<LifecyclePlugin>("b", calls));
  plugins.add(std::make_unique<LifecyclePlugin>("c", calls, true));
  plugins.add(std::make_unique<LifecyclePlugin>("d", calls));
  PluginNodeStates states;
  try {
    states.create(node, plugins);
    FAIL();
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ("c", e.what());
  }
  EXPECT_EQ(std::vector<std::string>({"start:a", "start:b", "start:c", "stop:b", "stop:a"}), calls);
  EXPECT_EQ(nullptr, states.find("a"));
}

TEST(PluginTest, nodeStatesStopInReverseOrder) {
  LuxirNode node;
  std::vector<std::string> calls;
  PluginRegistry plugins;
  plugins.add(std::make_unique<LifecyclePlugin>("a", calls));
  plugins.add(std::make_unique<DummyPlugin>("stateless"));
  plugins.add(std::make_unique<LifecyclePlugin>("b", calls));
  {
    PluginNodeStates states;
    states.create(node, plugins);
    EXPECT_NE(nullptr, states.find("a"));
    EXPECT_EQ(nullptr, states.find("stateless"));
    EXPECT_EQ(nullptr, states.find("missing"));
  }
  EXPECT_EQ(std::vector<std::string>({"start:a", "start:b", "stop:b", "stop:a"}), calls);
}

TEST(PluginTest, paramReaderUsesParamErrors) {
  ParamReader params("component", ParamReader::Entries{});
  EXPECT_EQ(7, params.getInt("count", 7));
  try {
    params.requireInt("count");
    FAIL();
  } catch (const ParamError& e) {
    EXPECT_STREQ("component: 'count' is required", e.what());
  }
  EXPECT_THROW(params.requireInt("count"), std::invalid_argument);
}
