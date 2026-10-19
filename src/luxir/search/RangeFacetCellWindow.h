// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <span>
#include <type_traits>

#include "luxir/search/RangeFacetGrid.h"
#include "luxir/search/RequestMemTracker.h"
#include "luxir/util/AtomicMerger.h"

namespace luxir {

// One contributor's canonical cell directory. Payloads are scalar counts or
// PAGED row pointers. The grid must outlive every contributor.
// Spare cells are capacity, not output extent. Occupancy includes zero-weight
// observations; widening adds output cells without manufacturing occupancy.
// EXPLICIT continues to use MergeableFixedBuckets, with its fixed hot loop.
template <typename T>
class RangeFacetCellWindow : public MergeableData {
  RequestMemResource resource;
  const RangeFacetGrid& grid;
  size_t cap;
  std::pmr::vector<T> counters;
  // Empty for affine windows, otherwise one lower key per cell plus its end.
  std::pmr::vector<int64_t> fences;
  int64_t base = 0;
  uint64_t width = 0;
  uint64_t gap = 0;
  size_t occupiedFirst = SIZE_MAX;
  size_t occupiedLast = 0;
  std::optional<std::pair<int64_t, int64_t>> widened;
  std::pmr::vector<RangeFacetRun> runs;
  RangeFacetRunCursor cursor;

  size_t indexOfKey(int64_t key) const;
  int64_t key(size_t index) const {
    return gap ? (int64_t)((uint64_t)base + index * gap) : fences[index];
  }
  void rebuild(int64_t lo, int64_t hi, bool left, const RangeFacetCellWindow* other = nullptr);
  bool rebuildAffine(int64_t lo, int64_t hi, bool left, uint64_t newGap);
  void install(std::pmr::vector<T>& counts, std::pmr::vector<int64_t>& keys,
               int64_t newBase, uint64_t newWidth, uint64_t newGap);
  [[gnu::cold, gnu::noinline]] size_t grow(int64_t value);
  void prepareMerge(const RangeFacetCellWindow& other);

public:
  explicit RangeFacetCellWindow(const RangeFacetGrid& grid, RequestMemTracker& tracker,
                                std::string_view label, size_t cap = 100000);
  RangeFacetCellWindow(const RangeFacetCellWindow&) = delete;
  RangeFacetCellWindow& operator=(const RangeFacetCellWindow&) = delete;

  // Only unsigned range arithmetic and division/search on a hit. An offset
  // spanning all 2^64 values has width=0 and deliberately takes the cold path.
  [[gnu::always_inline]] size_t observe(int64_t value) {
    size_t index;
    if (gap) {
      uint64_t offset = (uint64_t)value - (uint64_t)base;
      if (offset >= width) [[unlikely]] index = grow(value);
      else index = (size_t)(offset / gap);
    } else if (auto found = cursor.cachedBucket(value)) [[likely]] {
      // A run hit also certifies membership in the directory. An empty cache
      // has width zero, so floating/month grids fall through without a flag.
      index = *found;
    } else {
      uint64_t offset = (uint64_t)value - (uint64_t)base;
      if (offset >= width) [[unlikely]] index = grow(value);
      else if (!runs.empty()) index = cursor.seek(value, runs);
      else index = (size_t)(std::upper_bound(fences.begin(), fences.end(), value) - fences.begin() - 1);
    }
    occupiedFirst = std::min(occupiedFirst, index);
    occupiedLast = std::max(occupiedLast, index);
    return index;
  }
  void add(int64_t value, int64_t count = 1) requires std::is_same_v<T, int64_t> {
    counters[observe(value)] += count;
  }
  T& entry(size_t index) { return counters[index]; }
  std::span<T> entries() { return counters; }
  std::span<const T> entries() const { return counters; }
  int64_t cellKey(size_t index) const { return key(index); }
  std::pair<size_t, size_t> extent() const;
  size_t size() const { return empty() ? 0 : extent().second - extent().first + 1; }

  bool empty() const { return occupiedFirst == SIZE_MAX && !widened; }
  std::span<const T> counts() const {
    if (empty()) return {};
    auto [first, last] = extent();
    return std::span<const T>(counters).subspan(first, last - first + 1);
  }
  int64_t lowerFence(size_t index) const { return key(extent().first + index); }
  std::optional<std::pair<int64_t, int64_t>> occupiedBounds() const {
    if (occupiedFirst == SIZE_MAX) return std::nullopt;
    return std::pair{key(occupiedFirst), key(occupiedLast)};
  }
  size_t capacity() const { return counters.size(); }
  size_t residentBytes() const { return resource.bytes(); }
  bool affine() const { return gap != 0; }

  // Optional widening bounds use column encodings; end is exclusive.
  // An empty window needs both bounds to acquire an output extent.
  void widen(std::optional<int64_t> start, std::optional<int64_t> end);
  // Align the shared layout, then let the payload owner perform its union.
  template <typename Merge>
  void mergeFrom(const RangeFacetCellWindow& other, Merge&& merge) {
    assert(this != &other);
    if (other.empty()) return;
    prepareMerge(other);
    auto [otherFirst, otherLast] = other.extent();
    // Payload union can allocate and throw. Commit the raw extent first so
    // any partially merged owning payload remains inside it on later growth.
    if (auto occupied = other.occupiedBounds()) {
      occupiedFirst = std::min(occupiedFirst, indexOfKey(occupied->first));
      occupiedLast = std::max(occupiedLast, indexOfKey(occupied->second));
    }
    if (other.widened) {
      if (widened) widened = {std::min(widened->first, other.widened->first), std::max(widened->second, other.widened->second)};
      else widened = other.widened;
    }
    if (gap && gap == other.gap) {
      size_t offset = indexOfKey(other.key(otherFirst));
      for (size_t i = otherFirst; i <= otherLast; i++) merge(counters[offset + i - otherFirst], other.counters[i], key(offset + i - otherFirst));
    } else {
      size_t destination = indexOfKey(other.key(otherFirst));
      for (size_t i = otherFirst; i <= otherLast; i++) {
        int64_t sourceKey = other.key(i);
        if (gap) destination = indexOfKey(sourceKey);
        else while (fences[destination] < sourceKey) destination++;
        assert(gap || fences[destination] == sourceKey);
        merge(counters[destination], other.counters[i], sourceKey);
      }
    }
  }
  void mergeFrom(const RangeFacetCellWindow& other) requires std::is_same_v<T, int64_t> {
    mergeFrom(other, [](int64_t& a, int64_t b, int64_t) { a += b; });
  }
  static RangeFacetCellWindow* merge(RangeFacetCellWindow* a, RangeFacetCellWindow* b) requires std::is_same_v<T, int64_t> {
    if (a->counts().size() < b->counts().size()) std::swap(a, b);
    a->mergeFrom(*b);
    return a;
  }
};

extern template class RangeFacetCellWindow<int64_t>;
extern template class RangeFacetCellWindow<uint64_t**>;

} // namespace luxir
