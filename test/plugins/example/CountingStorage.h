// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

namespace luxir::example {

// Call counters kept by the example plugin's storage components, so a test can
// see that a node really ran on them.
//
// source: "backend" (the counting_ram backend, directory view opens) or
//         "layer" (the counting layer, file opens and creates).
// op:     "open" or "create".
// path: the directory view path (node/ prefix for node-private), or empty for the total.
struct CountingStorage {
  static void reset();
  static void add(std::string_view source, std::string_view op, std::string_view path);
  static uint64_t count(std::string_view source, std::string_view op, std::string_view path = {});
};

}  // namespace luxir::example
