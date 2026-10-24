// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/analysis/AnalysisRegistry.h"
#include "luxir/plugin/Plugin.h"

namespace luxir {

// The process-wide registries. Populated once by initPlugins(), then frozen.
class Registries {
public:
  PluginRegistry plugins;
  AnalysisRegistry analysis;

  static Registries& global();
  // Freezes every registry.
  void freeze();
};

}  // namespace luxir
