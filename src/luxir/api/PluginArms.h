// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <string_view>

namespace luxir::api {

// The plugin query and search op names the JSON dialect reads as arm keys:
// {"<name>": {...}} becomes a PluginQuery or PluginOp when a plugin registered
// that name, while any other unknown key stays an unknown_key error. The wire
// model cannot depend on the plugin registries, so initPlugins publishes them
// here before the registries freeze; null means no plugin registered any.
struct PluginArmNames {
  bool (*isQuery)(std::string_view name) = nullptr;
  bool (*isOp)(std::string_view name) = nullptr;
};

inline PluginArmNames pluginArmNames;

inline bool isPluginQueryArm(std::string_view name) {
  return pluginArmNames.isQuery != nullptr && pluginArmNames.isQuery(name);
}

inline bool isPluginOpArm(std::string_view name) {
  return pluginArmNames.isOp != nullptr && pluginArmNames.isOp(name);
}

} // namespace luxir::api
