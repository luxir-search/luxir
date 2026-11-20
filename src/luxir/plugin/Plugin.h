// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/server/PluginNodeState.h"

namespace luxir {

struct PluginInfo {
  std::string name;         // registry key, [a-z0-9_]+, unique
  std::string version;      // free text
  std::string description;  // one line
};

struct Registries;
class LuxirNode;

// A compiled-in extension. Plugins are linked into the executable at build time
// (see cmake/LuxirPlugins.cmake); there is no dynamic loading.
class Plugin {
public:
  virtual ~Plugin() = default;
  virtual PluginInfo info() const = 0;
  // Called once during initPlugins(). Register components into `r`.
  // Throwing aborts startup with the message.
  virtual void registerComponents(Registries& r) = 0;
  // Called per node after its collections are open. Null means no node state.
  // A throw aborts node construction; RAII must clean up partial work.
  // Both modes create states; node.getMode() distinguishes SERVE and PULL.
  virtual std::unique_ptr<PluginNodeState> createNodeState(LuxirNode&) { return nullptr; }
};

class PluginRegistry {
  std::vector<std::unique_ptr<Plugin>> list;
  bool isFrozen = false;

public:
  PluginRegistry();
  static PluginRegistry& global();
  // Throws std::invalid_argument on a bad or duplicate name, std::logic_error after freeze.
  void add(std::unique_ptr<Plugin> plugin);
  const std::vector<std::unique_ptr<Plugin>>& plugins() const { return list; }
  const Plugin* find(std::string_view name) const;
  bool frozen() const { return isFrozen; }
  void freeze() { isFrozen = true; }
};

// Defined in the generated translation unit (cmake/LuxirPlugins.cmake), never in
// luxir_lib: adds every configured plugin to `r`.
void addConfiguredPlugins(PluginRegistry& r);

// Idempotent. Calls addPlugins (executables pass addConfiguredPlugins, so the
// generated object is referenced from the executable and pulled in naturally), then runs
// each plugin's registerComponents, logs the plugin list, then freezes the global registries.
// Each area seeds its built-ins at registry construction. Call before any LuxirNode or Schema exists.
// A null adder means no plugins. Repeating the same adder is a no-op; a different
// adder is an error. Initialization failure is terminal: later calls rethrow the
// original exception. No registry state may be relied on afterwards; the process must exit.
void initPlugins(void (*addPlugins)(PluginRegistry&));

}  // namespace luxir

// Plugin entry point, once per plugin in one .cpp. The generated registration
// translation unit calls the factory this defines.
#define LUXIR_PLUGIN(name, ClassName) \
  std::unique_ptr<::luxir::Plugin> luxir_plugin_create_##name() { return std::make_unique<ClassName>(); }
