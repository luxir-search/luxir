// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/plugin/Registries.h"

namespace {

// Test-only plugin that exercises the plugin framework. Later steps register
// analysis components and storage backends here.
class ExamplePlugin : public luxir::Plugin {
public:
  luxir::PluginInfo info() const override { return {"example", "1.0", "exercises the plugin framework"}; }
  void registerComponents(luxir::Registries&) override {}
};

}  // namespace

LUXIR_PLUGIN(example, ExamplePlugin)
