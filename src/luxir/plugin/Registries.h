// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/plugin/Plugin.h"
#include "luxir/query/QueryRegistry.h"
#include "luxir/search/SearchOpRegistry.h"
#include "luxir/server/HttpRouteRegistry.h"

namespace luxir {

// References to area-owned registries, populated by initPlugins() then frozen.
struct Registries {
  PluginRegistry& plugins;
  // Plugin query types (api::PluginQuery).
  QueryRegistry& query;
  // Plugin search operations (api::PluginOp).
  SearchOpRegistry& searchOps;
  // Plugin HTTP routes.
  HttpRouteRegistry& http;

  static Registries global();
  // Freezes every registry.
  void freeze();
};

}  // namespace luxir
