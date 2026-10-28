// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/plugin/Plugin.h"

#include <stdexcept>

#include "luxir/api/PluginArms.h"
#include "luxir/plugin/PluginInit.h"
#include "luxir/plugin/Registries.h"
#include "luxir/util/log.h"

namespace luxir {

void PluginRegistry::add(std::unique_ptr<Plugin> plugin) {
  if (isFrozen) throw std::logic_error("plugin registry is frozen; plugins must be added during startup");
  if (!plugin) throw std::invalid_argument("null plugin");
  std::string name = plugin->info().name;
  if (name.empty()) throw std::invalid_argument("plugin name must not be empty");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      throw std::invalid_argument("invalid plugin name '" + name + "'; use [a-z0-9_]+");
  }
  if (find(name)) throw std::invalid_argument("duplicate plugin '" + name + "'");
  list.push_back(std::move(plugin));
}

const Plugin* PluginRegistry::find(std::string_view name) const {
  for (const auto& p : list) {
    if (p->info().name == name) return p.get();
  }
  return nullptr;
}

PluginRegistry::PluginRegistry() = default;

PluginRegistry& PluginRegistry::global() {
  static PluginRegistry instance;
  return instance;
}

Registries Registries::global() {
  return {PluginRegistry::global(), QueryRegistry::global(), SearchOpRegistry::global(), HttpRouteRegistry::global()};
}

void Registries::freeze() {
  plugins.freeze();
  query.freeze();
  searchOps.freeze();
  http.freeze();
}

void PluginRegistry::notifyNodeStarted(LuxirNode& node) {
  size_t started = 0;
  try {
    for (; started < list.size(); started++) list[started]->nodeStarted(node);
  } catch (...) {
    while (started > 0) list[--started]->nodeStopping(node);
    throw;
  }
}

void PluginRegistry::notifyNodeStopping(LuxirNode& node) noexcept {
  for (auto it = list.rbegin(); it != list.rend(); ++it) (*it)->nodeStopping(node);
}

void initPlugins(void (*addPlugins)(PluginRegistry&)) {
  static detail::PluginInit state;
  state.run(addPlugins, [](auto addPlugins) {
    Registries r = Registries::global();
    addPlugins(r.plugins);
    std::string names;
    for (const auto& p : r.plugins.plugins()) {
      p->registerComponents(r);
      PluginInfo info = p->info();
      if (!names.empty()) names += ", ";
      names += info.name + " " + info.version;
    }
    LOG_INFO("plugins: {}", names.empty() ? "none" : names);
    // The JSON dialect reads registered names as plugin arms (api/PluginArms.h).
    api::pluginArmNames.isQuery = [](std::string_view name) {
      return QueryRegistry::global().find(name) != nullptr;
    };
    api::pluginArmNames.isOp = [](std::string_view name) {
      return SearchOpRegistry::global().find(name) != nullptr;
    };
    r.freeze();
  });
}

}  // namespace luxir
