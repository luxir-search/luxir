// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/plugin/Plugin.h"

#include <mutex>
#include <stdexcept>

#include "luxir/api/PluginArms.h"
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

void notifyNodeStarted(LuxirNode& node) {
  for (const auto& p : PluginRegistry::global().plugins()) p->nodeStarted(node);
}

void notifyNodeStopping(LuxirNode& node) noexcept {
  const auto& list = PluginRegistry::global().plugins();
  for (auto it = list.rbegin(); it != list.rend(); ++it) (*it)->nodeStopping(node);
}

void initPlugins(void (*addPlugins)(PluginRegistry&)) {
  static std::once_flag once;
  std::call_once(once, [addPlugins] {
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
