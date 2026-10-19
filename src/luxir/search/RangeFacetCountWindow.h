// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <span>

#include "luxir/search/RangeFacetGrid.h"
#include "luxir/search/RequestMemTracker.h"
#include "luxir/util/AtomicMerger.h"

namespace luxir {

// One contributor's DATA counts. The grid must outlive every contributor.
// Spare cells are capacity, not output extent. Occupancy includes zero-weight
// observations; widening adds output cells without manufacturing occupancy.
// EXPLICIT continues to use MergeableFixedBuckets, with its fixed hot loop.
class RangeFacetCountWindow : public MergeableData {
  RequestMemResource resource;
  const RangeFacetGrid& grid;
  size_t cap;
  std::pmr::vector<int64_t> counters;
  // Empty for affine windows, otherwise one lower key per cell plus its end.
  std::pmr::vector<int64_t> fences;
  int64_t base = 0;
  uint64_t width = 0;
  uint64_t gap = 0;
  size_t occupiedFirst = SIZE_MAX;
  size_t occupiedLast = 0;
  std::optional<std::pair<int64_t, int64_t>> widened;

  std::pair<size_t, size_t> extent() const;
  size_t indexOfKey(int64_t key) const;
  int64_t key(size_t index) const {
    return gap ? (int64_t)((uint64_t)base + index * gap) : fences[index];
  }
  void rebuild(int64_t lo, int64_t hi, bool left, const RangeFacetCountWindow* other = nullptr);
  bool rebuildAffine(int64_t lo, int64_t hi, bool left, uint64_t newGap);
  void install(std::pmr::vector<int64_t>& counts, std::pmr::vector<int64_t>& keys,
               int64_t newBase, uint64_t newWidth, uint64_t newGap);
  [[gnu::cold, gnu::noinline]] size_t grow(int64_t value);

public:
  explicit RangeFacetCountWindow(const RangeFacetGrid& grid, RequestMemTracker& tracker,
                                std::string_view label, size_t cap = 100000);
  RangeFacetCountWindow(const RangeFacetCountWindow&) = delete;
  RangeFacetCountWindow& operator=(const RangeFacetCountWindow&) = delete;

  // Only unsigned range arithmetic and division/search on a hit. An offset
  // spanning all 2^64 values has width=0 and deliberately takes the cold path.
  void add(int64_t value, int64_t count = 1) {
    uint64_t offset = (uint64_t)value - (uint64_t)base;
    size_t index;
    if (offset >= width) [[unlikely]] index = grow(value);
    else if (gap) index = (size_t)(offset / gap);
    else index = (size_t)(std::upper_bound(fences.begin(), fences.end(), value) - fences.begin() - 1);
    occupiedFirst = std::min(occupiedFirst, index);
    occupiedLast = std::max(occupiedLast, index);
    counters[index] += count;
  }

  bool empty() const { return occupiedFirst == SIZE_MAX && !widened; }
  std::span<const int64_t> counts() const {
    if (empty()) return {};
    auto [first, last] = extent();
    return std::span<const int64_t>(counters).subspan(first, last - first + 1);
  }
  int64_t lowerFence(size_t index) const { return key(extent().first + index); }
  std::optional<std::pair<int64_t, int64_t>> occupiedBounds() const {
    if (occupiedFirst == SIZE_MAX) return std::nullopt;
    return std::pair{key(occupiedFirst), key(occupiedLast)};
  }
  size_t capacity() const { return counters.size(); }
  bool affine() const { return gap != 0; }

  // Optional widening bounds use column encodings; end is exclusive.
  // An empty window needs both bounds to acquire an output extent.
  void widen(std::optional<int64_t> start, std::optional<int64_t> end);
  void mergeFrom(const RangeFacetCountWindow& other);
  static RangeFacetCountWindow* merge(RangeFacetCountWindow* a, RangeFacetCountWindow* b) {
    if (a->counts().size() < b->counts().size()) std::swap(a, b);
    a->mergeFrom(*b);
    return a;
  }
};

} // namespace luxir
