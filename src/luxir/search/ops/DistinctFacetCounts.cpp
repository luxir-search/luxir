// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "DistinctFacetCounts.h"

namespace luxir {
DistinctFacetMode forcedDistinctFacetMode = [] {
  const char* text = std::getenv("LUXIR_DISTINCT_REDUCER");
  if (text != nullptr) {
    std::string_view value(text);
    if (value == "paged") return DistinctFacetMode::PAGED;
    if (value == "hash") return DistinctFacetMode::HASH;
  }
  return DistinctFacetMode::AUTO;
}();
} // namespace luxir
