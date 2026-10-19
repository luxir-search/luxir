// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/RangeFacetCellWindow.h"

#include <stdexcept>
#include "luxir/util/ApiError.h"

namespace luxir {
namespace {
[[noreturn]] void capExceeded() {
  throw RequestError("range exceeds the 100000 bucket limit; use a larger gap");
}
}

template <typename T>
RangeFacetCellWindow<T>::RangeFacetCellWindow(const RangeFacetGrid& grid,
    RequestMemTracker& tracker, std::string_view label, size_t cap)
    : resource(tracker, label, std::is_same_v<T, int64_t> ? "facet range counts" : "facet distinct state"), grid(grid), cap(cap),
      counters(&resource), fences(&resource), runs(&resource) {
  assert(cap != 0 && cap <= 100000);
}

template <typename T>
size_t RangeFacetCellWindow<T>::indexOfKey(int64_t value) const {
  if (gap) return (size_t)(((uint64_t)value - (uint64_t)base) / gap);
  auto it = std::lower_bound(fences.begin(), fences.end(), value);
  assert(it != fences.end() && *it == value);
  return (size_t)(it - fences.begin());
}

template <typename T>
std::pair<size_t, size_t> RangeFacetCellWindow<T>::extent() const {
  size_t first = occupiedFirst, last = occupiedLast;
  if (widened) {
    first = std::min(first, indexOfKey(widened->first));
    last = std::max(last, indexOfKey(widened->second));
  }
  return {first, last};
}

template <typename T>
void RangeFacetCellWindow<T>::install(std::pmr::vector<T>& counts,
    std::pmr::vector<int64_t>& keys, int64_t newBase, uint64_t newWidth, uint64_t newGap) {
  // Complete all allocations before rebasing owning payloads or committing any
  // directory. The old and new run directories share the relocation charge.
  auto newRuns = grid.runs(keys, &resource);
  auto newIndex = [&](int64_t value) {
    return newGap ? (size_t)(((uint64_t)value - (uint64_t)newBase) / newGap)
        : (size_t)(std::lower_bound(keys.begin(), keys.end(), value) - keys.begin());
  };
  if (!empty()) {
    auto [first, last] = extent();
    if (gap && newGap == gap) {
      size_t offset = newIndex(key(first));
      std::copy(counters.begin() + first, counters.begin() + last + 1, counts.begin() + offset);
    } else {
      // Copy by canonical key when either window is non-affine. Nominal
      // civil indices do not identify the two occurrences of a folded hour.
      size_t destination = newIndex(key(first));
      for (size_t i = first; i <= last; i++) {
        int64_t oldKey = key(i);
        if (newGap) destination = newIndex(oldKey);
        else while (keys[destination] < oldKey) destination++;
        assert(newGap || keys[destination] == oldKey);
        counts[destination] = counters[i];
      }
    }
  }
  size_t newFirst = occupiedFirst == SIZE_MAX ? SIZE_MAX : newIndex(key(occupiedFirst));
  size_t newLast = occupiedFirst == SIZE_MAX ? 0 : newIndex(key(occupiedLast));
  // Both old and new allocations remain charged until these locals die.
  counters.swap(counts);
  fences.swap(keys);
  runs.swap(newRuns);
  cursor.cached = runs.empty() ? RangeFacetRun{} : runs.front();
  base = newBase;
  width = newWidth;
  gap = newGap;
  occupiedFirst = newFirst;
  occupiedLast = newLast;
}

template <typename T>
bool RangeFacetCellWindow<T>::rebuildAffine(int64_t lo, int64_t hi, bool left, uint64_t newGap) {
  uint64_t distance = (uint64_t)hi - (uint64_t)lo;
  if (distance / newGap >= cap) capExceeded();
  size_t count = (size_t)(distance / newGap) + 1;
  size_t target = std::min(cap, std::max({size_t{8}, counters.size() * 2, count + count / 2}));
  auto bounds = grid.valueBounds();
  __int128 min = bounds.first, max = bounds.second;
  if (lo < min || hi > max) throw RequestError("range fence is outside the supported civil range");
  size_t extra = target - count;
  // Bias slack toward the growing end, retaining slack at the other end too.
  // Giving it all to one side degenerates into repeated full relocations for
  // alternating-end input once capacity reaches the bucket ceiling.
  size_t before = left ? extra - extra / 4 : extra / 4;
  size_t after = extra - before;
  before = (size_t)std::min((__int128)before, ((__int128)lo - min) / newGap);
  after = (size_t)std::min((__int128)after, (max - hi) / newGap);
  extra = before + after;
  int64_t first = (int64_t)((__int128)lo - (__int128)before * newGap);
  int64_t last = (int64_t)((__int128)hi + (__int128)after * newGap);
  __int128 end = std::min((__int128)last + newGap, max + 1);
  // Certify the entire last cell too: a transition inside it can insert a
  // fence even when all the proposed lower keys lie on the affine phase.
  std::optional<int64_t> certified;
  try { certified = grid.affineGap(first, (int64_t)std::min(end, (__int128)INT64_MAX)); }
  catch (const std::runtime_error&) {
    // Headroom may cross a tzdb/representation boundary even though every
    // observed value fits. Retry the actual extent before rejecting it.
    first = lo;
    last = hi;
    extra = 0;
    end = std::min((__int128)last + newGap, max + 1);
    certified = grid.affineGap(first, (int64_t)std::min(end, (__int128)INT64_MAX));
  }
  if (!certified || (uint64_t)*certified != newGap) return false;
  std::pmr::vector<T> counts(count + extra, T{}, &resource);
  std::pmr::vector<int64_t> keys(&resource);
  install(counts, keys, first, (uint64_t)(end - first), newGap);
  return true;
}

template <typename T>
void RangeFacetCellWindow<T>::rebuild(int64_t lo, int64_t hi, bool left,
                                  const RangeFacetCellWindow<T>* other) {
  auto certified = grid.affineGap(lo, hi == INT64_MAX ? hi : hi + 1);
  if (certified && rebuildAffine(lo, hi, left, (uint64_t)*certified)) return;

  std::pmr::vector<int64_t> keys(&resource);
  keys.reserve(std::min(cap + 1, std::max({size_t{9}, counters.size() * 2 + 1,
      other ? other->capacity() + 1 : 0})));
  auto cell = grid.floor(lo);
  size_t cursor = 0, otherCursor = 0;
  bool cellIndexKnown = true;
  auto cachedNext = [&](const RangeFacetCellWindow<T>& window, size_t& index) -> std::optional<int64_t> {
    if (window.counters.empty() || cell.key < window.base) return std::nullopt;
    if (window.gap) {
      uint64_t offset = (uint64_t)cell.key - (uint64_t)window.base;
      // Reuse only complete cells, not an affine end clipped at INT64_MAX.
      if (offset >= window.width || window.width - offset < window.gap) return std::nullopt;
      if (cell.key > INT64_MAX - (int64_t)window.gap) return std::nullopt;
      return (int64_t)((uint64_t)cell.key + window.gap);
    }
    // A linear merge of sorted fence streams; each cursor advances at most
    // once per source fence over the entire rebuild.
    while (index < window.fences.size() && window.fences[index] < cell.key) index++;
    if (index + 1 >= window.fences.size() || window.fences[index] != cell.key) return std::nullopt;
    return window.fences[++index];
  };
  auto advance = [&](bool required = true) {
    auto known = cachedNext(*this, cursor);
    if (!known && other) known = cachedNext(*other, otherCursor);
    if (known) {
      cell = {*known};
      cellIndexKnown = false;
    } else {
      if (!required) return false;
      if (!cellIndexKnown) cell = grid.floor(cell.key);
      cell = grid.next(cell);
      cellIndexKnown = true;
    }
    return true;
  };
  keys.push_back(cell.key);
  do {
    if (keys.size() > cap) capExceeded();
    advance();
    keys.push_back(cell.key);
  } while (cell.key <= hi);
  size_t actual = keys.size() - 1;
  size_t target = std::min(cap, std::max({size_t{8}, counters.size() * 2, actual + actual / 2}));
  keys.reserve(target + 1);
  size_t extra = target - actual;
  size_t before = left ? extra - extra / 4 : extra / 4;
  size_t after = extra - before;
  {
    std::pmr::vector<int64_t> prefix(&resource);
    size_t back = fences.size(), otherBack = other ? other->fences.size() : 0;
    auto cachedPrevious = [&](const RangeFacetCellWindow<T>& window, size_t& index,
                               int64_t first) -> std::optional<int64_t> {
      if (window.counters.empty() || first <= window.base) return std::nullopt;
      if (window.gap) {
        uint64_t offset = (uint64_t)first - (uint64_t)window.base;
        if (offset > window.width || offset % window.gap != 0) return std::nullopt;
        return first - (int64_t)window.gap;
      }
      while (index > 0 && window.fences[index - 1] >= first) index--;
      if (index == 0 || index >= window.fences.size() || window.fences[index] != first) return std::nullopt;
      return window.fences[index - 1];
    };
    while (prefix.size() < before) {
      int64_t first = prefix.empty() ? lo : prefix.back();
      if (first == INT64_MIN) break;
      auto previous = cachedPrevious(*this, back, first);
      if (!previous && other) previous = cachedPrevious(*other, otherBack, first);
      if (!previous) {
        // A merge can reuse known headroom, but need not resolve speculative
        // cells beyond either contributor. Later observations grow it once.
        if (other) break;
        try { previous = grid.floor(first - 1).key; }
        catch (const std::runtime_error&) { break; } // Optional headroom at a representation boundary.
      }
      prefix.push_back(*previous);
    }
    keys.insert(keys.begin(), prefix.rbegin(), prefix.rend());
  }
  for (size_t i = 0; i < after; i++) {
    try { if (!advance(other == nullptr)) break; }
    catch (const std::runtime_error&) { break; }
    keys.push_back(cell.key);
  }
  std::pmr::vector<T> counts(keys.size() - 1, T{}, &resource);
  int64_t first = keys.front();
  uint64_t newWidth = (uint64_t)keys.back() - (uint64_t)first;
  install(counts, keys, first, newWidth, 0);
}

template <typename T>
size_t RangeFacetCellWindow<T>::grow(int64_t value) {
  auto bounds = grid.valueBounds();
  if (value < bounds.first || value > bounds.second) {
    throw RequestError("range fence is outside the supported civil range");
  }
  auto cell = grid.floor(value);
  // A full-int64 affine window has an unrepresentable unsigned width. Its
  // lookup is cold, but observations inside it need no relocation.
  if (!counters.empty() && cell.key >= base && cell.key <= key(counters.size() - 1)) {
    return indexOfKey(cell.key);
  }
  int64_t lo = cell.key, hi = cell.key;
  if (!empty()) {
    auto [first, last] = extent();
    lo = std::min(lo, key(first));
    hi = std::max(hi, key(last));
  }
  rebuild(lo, hi, cell.key < base);
  return indexOfKey(cell.key);
}

template <typename T>
void RangeFacetCellWindow<T>::widen(std::optional<int64_t> start, std::optional<int64_t> end) {
  if (!start && !end) return;
  if (empty() && (!start || !end || *start >= *end)) return;
  int64_t lo, hi;
  if (empty()) {
    lo = grid.floor(*start).key;
    hi = grid.floor(*end - 1).key; // start < end proves this cannot underflow.
  } else {
    auto [first, last] = extent();
    lo = key(first);
    hi = key(last);
    if (start && *start < lo) lo = grid.floor(*start).key;
    if (end && *end > hi) hi = std::max(hi, grid.floor(*end - 1).key);
  }
  rebuild(lo, hi, lo < base);
  widened = {lo, hi};
}

template <typename T>
void RangeFacetCellWindow<T>::prepareMerge(const RangeFacetCellWindow<T>& other) {
  assert(&grid == &other.grid && this != &other);
  if (other.empty()) return;
  auto [otherFirst, otherLast] = other.extent();
  int64_t lo = other.key(otherFirst), hi = other.key(otherLast);
  if (!empty()) {
    auto [first, last] = extent();
    lo = std::min(lo, key(first));
    hi = std::max(hi, key(last));
  }
  // In-window unions fit because even capacity (including headroom) is capped.
  if (counters.empty() || lo < base || hi > key(counters.size() - 1)) rebuild(lo, hi, lo < base, &other);
}

template class RangeFacetCellWindow<int64_t>;
template class RangeFacetCellWindow<uint64_t**>;

} // namespace luxir
