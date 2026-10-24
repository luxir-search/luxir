// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/plugin/Plugin.h"

#include <mutex>
#include <stdexcept>

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

Registries& Registries::global() {
  static Registries instance;
  return instance;
}

void Registries::freeze() {
  plugins.freeze();
  analysis.freeze();
}

void initPlugins(void (*addPlugins)(PluginRegistry&)) {
  static std::once_flag once;
  std::call_once(once, [addPlugins] {
    Registries& r = Registries::global();
    registerBuiltinAnalysis(r.analysis);
    addPlugins(r.plugins);
    std::string names;
    for (const auto& p : r.plugins.plugins()) {
      p->registerComponents(r);
      PluginInfo info = p->info();
      if (!names.empty()) names += ", ";
      names += info.name + " " + info.version;
    }
    LOG_INFO("plugins: {}", names.empty() ? "none" : names);
    r.freeze();
  });
}

}  // namespace luxir
