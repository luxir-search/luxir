// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

namespace luxir::example {

// Call counters kept by the example plugin's storage components, so a test can
// see that a node really ran on them.
//
// source: "backend" (the counting_ram backend, directory opens and creates) or
//         "layer" (the counting layer, file opens and creates).
// op:     "open" or "create".
// collection: a CollectionId label, or empty for the total over collections.
struct CountingStorage {
  static void reset();
  static void add(std::string_view source, std::string_view op, std::string_view collection);
  static uint64_t count(std::string_view source, std::string_view op, std::string_view collection = {});
};

}  // namespace luxir::example
