// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/analysis/AnalysisRegistry.h"
#include "luxir/index/MergePolicyRegistry.h"
#include "luxir/index/SegmentOverlayRegistry.h"
#include "luxir/plugin/Plugin.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/query/QueryRegistry.h"
#include "luxir/search/SearchOpRegistry.h"
#include "luxir/server/HttpRouteRegistry.h"

namespace luxir {

// References to area-owned registries, populated by initPlugins() then frozen.
struct Registries {
  PluginRegistry& plugins;
  AnalysisRegistry& analysis;
  // Storage backends and layers.
  StorageRegistry& storage;
  // Plugin query types (api::PluginQuery).
  QueryRegistry& query;
  // Plugin search operations (api::PluginOp).
  SearchOpRegistry& searchOps;
  // Plugin HTTP routes.
  HttpRouteRegistry& http;
  // Segment merge policies, selected per collection (SchemaDef.merge_policy).
  MergePolicyRegistry& mergePolicies;
  // Per-segment overlays, selected per collection (SchemaDef.segment_overlays).
  SegmentOverlayRegistry& segmentOverlays;

  static Registries global();
  // Freezes every registry.
  void freeze();
};

}  // namespace luxir
