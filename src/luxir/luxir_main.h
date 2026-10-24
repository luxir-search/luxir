// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

#include "luxir/plugin/Plugin.h"

int luxir_main(int argc, char** argv, void (*addPlugins)(luxir::PluginRegistry&));
std::string luxir_banner();  // return a banner string that includes useful version info
