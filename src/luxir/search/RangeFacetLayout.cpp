// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/RangeFacetLayout.h"

namespace luxir {

size_t RangeFacetRunCursor::seek(int64_t value, std::span<const RangeFacetRun> runs) {
  auto found = std::upper_bound(runs.begin(), runs.end(), value,
      [](int64_t key, const RangeFacetRun& run) { return key < run.first; }) - 1;
  cached = *found;
  return cached.bucket + (size_t)(((uint64_t)value - (uint64_t)cached.first) / cached.gap);
}

} // namespace luxir
