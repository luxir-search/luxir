// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

namespace luxir {

// A contiguous set of cells with equal encoded width. Short transition cells
// and clipped endpoints are runs too; canonical fences remain authoritative.
struct RangeFacetRun {
  int64_t first;
  uint64_t width;
  uint64_t gap;
  size_t bucket;
};

// Call/contributor-local cache. Neither the shared grid nor a request op owns
// mutable lookup state. The binary-search fallback serves calendar months and
// floating grids, which have no run directory.
struct RangeFacetRunCursor {
  RangeFacetRun cached{};

  [[gnu::noinline]] size_t seek(int64_t value, std::span<const RangeFacetRun> runs);

  [[gnu::always_inline]] std::optional<size_t> cachedBucket(int64_t value) const {
    uint64_t offset = (uint64_t)value - (uint64_t)cached.first;
    if (offset >= cached.width) return std::nullopt;
    return cached.bucket + (size_t)(offset / cached.gap);
  }

  [[gnu::always_inline]] size_t bucketOf(int64_t value, std::span<const RangeFacetRun> runs) {
    if (auto bucket = cachedBucket(value)) [[likely]] return *bucket;
    return seek(value, runs);
  }

  [[gnu::always_inline]] size_t bucketOf(int64_t value, std::span<const RangeFacetRun> runs,
                  std::span<const int64_t> fences) {
    if (runs.empty()) return (size_t)(std::upper_bound(fences.begin(), fences.end(), value) - fences.begin() - 1);
    return bucketOf(value, runs);
  }
};

} // namespace luxir
