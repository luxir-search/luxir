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

DistinctFacetCounts::DistinctFacetCounts(RequestMemTracker& tracker, std::string_view label,
    DistinctFacetMode requested, const RangeFacetGrid& grid, FacetDimension groups,
    size_t pagedLimit, size_t bucketCap, bool isContiguous)
    : resource(tracker, label), mode(requested), values(), groups(groups),
      words(groups.size / 64 + (groups.size % 64 != 0)),
      pagesPerValue(words / PAGE_WORDS + (words % PAGE_WORDS != 0)), rows(&resource), pairs(&resource),
      nullValues(&resource), missingGroups(&resource), singletons(&resource), last(&resource), counts(&resource) {
  assert(pagedLimit != 0);
  window.reset(std::pmr::polymorphic_allocator<>(&resource).new_object<CellWindow>(grid, tracker, label, bucketCap));
  bool groupDirectory = groups.dense && groups.size != 0 && pagesPerValue <= (32ULL << 20) / sizeof(uint64_t*);
  if (isContiguous || distinctFacetRunContractForTests) mode = DistinctFacetMode::RUN;
  else if (requested == DistinctFacetMode::AUTO) {
    mode = forcedDistinctFacetMode == DistinctFacetMode::HASH || !groupDirectory
        ? DistinctFacetMode::HASH : DistinctFacetMode::PAGED;
    adaptive = mode == DistinctFacetMode::PAGED && forcedDistinctFacetMode != DistinctFacetMode::PAGED;
  } else if (requested == DistinctFacetMode::PAGED && !groupDirectory) mode = DistinctFacetMode::HASH;
  assert(mode != DistinctFacetMode::RUN || isContiguous || distinctFacetRunContractForTests);
  nextCheck = pagedLimit;
  if (distinctFacetStatsForTests) {
    if (mode == DistinctFacetMode::PAGED) distinctFacetStatsForTests->paged++;
    if (mode == DistinctFacetMode::HASH) distinctFacetStatsForTests->hash++;
    if (mode == DistinctFacetMode::RUN) distinctFacetStatsForTests->run++;
  }
}

uint64_t* DistinctFacetCounts::allocatePage(uint64_t**& row, uint64_t index) {
  assert(index < pagesPerValue);
  bool fresh = row == nullptr;
  size_t rowBytes = pagesPerValue * sizeof(uint64_t*);
  auto* directory = row;
  if (fresh) {
    directory = (uint64_t**)resource.allocate(rowBytes, alignof(uint64_t*));
    std::fill_n(directory, pagesPerValue, nullptr);
  }
  size_t bytes = pageWords(index) * sizeof(uint64_t);
  uint64_t* p;
  try { p = (uint64_t*)resource.allocate(bytes, alignof(uint64_t)); }
  catch (...) {
    if (fresh) resource.deallocate(directory, rowBytes, alignof(uint64_t*));
    throw;
  }
  std::fill_n(p, pageWords(index), 0);
  directory[index] = p;
  row = directory;
  if (window) pagedBytes += bytes + (fresh ? rowBytes : 0);
  return p;
}

void DistinctFacetCounts::releaseRow(uint64_t**& row) {
  if (!row) return;
  for (uint64_t i = 0; i < pagesPerValue; i++) if (row[i]) {
    size_t bytes = pageWords(i) * sizeof(uint64_t);
    resource.deallocate(row[i], bytes, alignof(uint64_t));
    if (window) pagedBytes -= bytes;
  }
  size_t bytes = pagesPerValue * sizeof(uint64_t*);
  resource.deallocate(row, bytes, alignof(uint64_t*));
  if (window) pagedBytes -= bytes;
  row = nullptr;
}

void DistinctFacetCounts::releasePages() {
  if (window) for (auto& row : window->entries()) releaseRow(row);
  else { for (auto& row : rows) releaseRow(row); rows.clear(); }
  releaseRow(missingRow);
  assert(pagedBytes == 0);
}

uint64_t* DistinctFacetCounts::growingPage(uint64_t**& row, uint64_t index) {
  size_t extra = pageWords(index) * sizeof(uint64_t) + (row ? 0 : pagesPerValue * sizeof(uint64_t*));
  if (adaptive) considerHash(extra);
  if (mode == DistinctFacetMode::HASH) return nullptr;
  return allocatePage(row, index);
}

void DistinctFacetCounts::considerHash(size_t extra) {
  if (extra < nextCheck && pagedBytes < nextCheck - extra) return;
  uint64_t count = 0;
  eachRow([&](int64_t, uint64_t** row) { count += rowCount(row); });
  count += rowCount(missingRow);
  // A conservative flat-set estimate includes its minimum allocation. Only
  // convert when temporary HASH state is at most an eighth of the old pages.
  // The margin also protects bitsets whose rows are still filling in. This
  // bounds conversion overlap and avoids converting moderately dense state.
  if (count && (__uint128_t)512 + (__uint128_t)count * 48 <= pagedBytes / 8) {
    convertToHash();
    return;
  }
  // Scan only at geometric allocation milestones, never per observation.
  nextCheck = std::max(nextCheck, pagedBytes);
  nextCheck = nextCheck > SIZE_MAX / 2 ? SIZE_MAX : nextCheck * 2;
}

void DistinctFacetCounts::convertToHash() {
  Set<FacetPair, FacetPairHash> converted(&resource);
  Set<int64_t> missing(&resource);
  size_t count = 0;
  eachRow([&](int64_t, uint64_t** row) { count += (size_t)rowCount(row); });
  converted.reserve(count);
  missing.reserve((size_t)rowCount(missingRow));
  eachRow([&](int64_t value, uint64_t** row) {
    eachGroup(row, [&](int64_t group) { converted.insert({value, group}); });
  });
  eachGroup(missingRow, [&](int64_t group) { missing.insert(group); });
  // Allocations and enumeration succeeded. Until here the PAGED state is
  // untouched, including on a request-breaker exception.
  pairs.swap(converted);
  missingGroups.swap(missing);
  releasePages();
  mode = DistinctFacetMode::HASH;
  conversions++;
}

DistinctFacetCounts* DistinctFacetCounts::mergeGrowing(DistinctFacetCounts* a, DistinctFacetCounts* b) {
  assert(a->window && b->window);
  assert((a->mode == DistinctFacetMode::RUN) == (b->mode == DistinctFacetMode::RUN));
  assert(a->groups.dense == b->groups.dense && a->groups.base == b->groups.base && a->groups.size == b->groups.size);
  if (b->mode == DistinctFacetMode::HASH && a->mode != DistinctFacetMode::HASH) std::swap(a, b);
  else if (a->mode == b->mode) {
    if (a->mode == DistinctFacetMode::HASH) {
      if ((!a->tuples.empty() || !b->tuples.empty()) ? a->tuples.size() < b->tuples.size() : a->pairs.size() < b->pairs.size()) std::swap(a, b);
    } else if (a->window->size() < b->window->size()) std::swap(a, b);
  }
  bool paged = a->mode == DistinctFacetMode::PAGED && b->mode == DistinctFacetMode::PAGED;
  auto unionRow = [&](uint64_t**& to, uint64_t** from, int64_t value, bool missing) {
    if (!from) return;
    for (uint64_t p = 0; p < a->pagesPerValue; p++) if (from[p]) {
      auto* target = to ? to[p] : nullptr;
      if (!target && a->mode == DistinctFacetMode::PAGED) target = a->growingPage(to, p);
      if (a->mode == DistinctFacetMode::HASH) {
        for (uint64_t w = 0; w < a->pageWords(p); w++) {
          uint64_t bits = from[p][w];
          while (bits) {
            int64_t group = a->groups.value((p * PAGE_WORDS + w) * 64 + std::countr_zero(bits));
            if (missing) a->missingGroups.insert(group);
            else a->pairs.insert({value, group});
            bits &= bits - 1;
          }
        }
      } else {
        assert(target);
        for (uint64_t w = 0; w < a->pageWords(p); w++) target[w] |= from[p][w];
      }
    }
  };
  a->window->mergeFrom(*b->window, [&](uint64_t**& to, uint64_t** from, int64_t key) {
    if (paged) unionRow(to, from, key, false);
  });
  if (paged) unionRow(a->missingRow, b->missingRow, 0, true);
  else if (b->mode == DistinctFacetMode::PAGED) {
    b->eachRow([&](int64_t value, uint64_t** row) {
      b->eachGroup(row, [&](int64_t group) { a->pairs.insert({value, group}); });
    });
    b->eachGroup(b->missingRow, [&](int64_t group) { a->missingGroups.insert(group); });
  } else if (a->mode == DistinctFacetMode::HASH) {
    std::pmr::vector<int64_t> remap(b->tuples.size(), 0, &a->resource);
    for (const auto& [key, id] : b->tuples) remap[(size_t)id] = a->intern(key);
    for (auto pair : b->pairs) {
      if (!remap.empty()) pair.group = remap[(size_t)pair.group];
      a->pairs.insert(pair);
    }
    for (auto group : b->missingGroups) a->missingGroups.insert(remap.empty() ? group : remap[(size_t)group]);
  } else {
    assert(a->mode == DistinctFacetMode::RUN && b->mode == DistinctFacetMode::RUN);
    for (auto [key, count] : b->counts) a->counts[key] += count;
    a->missingCount += b->missingCount;
  }
  a->nullValues.insert(b->nullValues.begin(), b->nullValues.end());
  a->missingNull |= b->missingNull;
  for (auto [value, state] : b->singletons) a->singletons[value].count += state.count;
  a->missingSingletons.count += b->missingSingletons.count;
  return a;
}
} // namespace luxir
