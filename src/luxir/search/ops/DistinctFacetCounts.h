// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <bit>
#include <cstring>
#include <type_traits>
#include <boost/unordered/unordered_flat_set.hpp>
#include "FacetGroupKeys.h"
#include "FacetCountResource.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/util/AtomicMerger.h"
#include "luxir/search/RangeFacetCellWindow.h"

namespace luxir {

enum class DistinctFacetMode { AUTO, PAGED, HASH, RUN };

struct FacetPair {
  int64_t value;
  int64_t group;
  bool operator==(const FacetPair&) const = default;
};
struct FacetPairHash {
  size_t operator()(const FacetPair& pair) const {
    size_t hash = boost::hash<int64_t>{}(pair.value);
    boost::hash_combine(hash, pair.group);
    return hash;
  }
};

// One binding supplies the domain identity. Ordinary pairs preserve both
// int64 identities until global union. Null keys and singleton documents are
// tagged separately; no int64 value is reserved as a sentinel.
class DistinctFacetCounts : public MergeableData {
  template <typename K, typename V>
  using Map = boost::unordered_flat_map<K, V, boost::hash<K>, std::equal_to<K>,
      std::pmr::polymorphic_allocator<std::pair<const K, V>>>;
  template <typename K, typename Hash = boost::hash<K>>
  using Set = boost::unordered_flat_set<K, Hash, std::equal_to<K>, std::pmr::polymorphic_allocator<K>>;
  struct SingletonCount { int64_t count = 0; uint64_t lastDoc = UINT64_MAX; };
  using CellWindow = RangeFacetCellWindow<uint64_t**>;
  struct WindowDeleter {
    std::pmr::memory_resource* resource;
    void operator()(CellWindow* window) const {
      std::pmr::polymorphic_allocator<>(resource).delete_object(window);
    }
  };

  FacetCountResource resource;
  DistinctFacetMode mode;
  FacetDimension values;
  FacetDimension groups;
  static constexpr uint64_t PAGE_WORDS = 64;
  uint64_t words;
  uint64_t pagesPerValue;
  std::pmr::vector<uint64_t**> rows;
  uint64_t** missingRow = nullptr;
  Set<FacetPair, FacetPairHash> pairs;
  Set<int64_t> nullValues;
  Set<int64_t> missingGroups;
  bool missingNull = false;
  Map<int64_t, SingletonCount> singletons;
  SingletonCount missingSingletons;
  Map<int64_t, int64_t> last;
  std::optional<int64_t> lastMissing;
  int64_t missingCount = 0;

  struct TupleHash {
    using is_transparent = void;
    size_t operator()(std::string_view key) const { return std::hash<std::string_view>{}(key); }
  };
  boost::unordered_flat_map<std::pmr::string, int64_t, TupleHash, std::equal_to<>,
      std::pmr::polymorphic_allocator<std::pair<const std::pmr::string, int64_t>>> tuples{&resource};
  std::pmr::string encoded{&resource};
  std::unique_ptr<CellWindow, WindowDeleter> window{nullptr, WindowDeleter{&resource}};
  bool adaptive = false;
  size_t pagedBytes = 0;
  size_t nextCheck = 32ULL << 20;
  size_t conversions = 0;

  int64_t intern(std::string_view key) {
    auto found = tuples.find(key);
    if (found != tuples.end()) return found->second;
    int64_t id = (int64_t)tuples.size();
    tuples.emplace(std::pmr::string(key, &resource), id);
    if (distinctFacetStatsForTests) distinctFacetStatsForTests->interned++;
    return id;
  }

  uint64_t pageWords(uint64_t index) const { return std::min(PAGE_WORDS, words - index * PAGE_WORDS); }
  [[gnu::cold, gnu::noinline]] uint64_t* allocatePage(uint64_t**& row, uint64_t index);
  uint64_t* page(uint64_t**& row, uint64_t index) {
    if (row == nullptr || row[index] == nullptr) [[unlikely]] return allocatePage(row, index);
    return row[index];
  }
  void releaseRow(uint64_t**& row);
  void releasePages();
  [[gnu::cold, gnu::noinline]] uint64_t* growingPage(uint64_t**& row, uint64_t index);
  void considerHash(size_t extra);
  void convertToHash();
  static DistinctFacetCounts* mergeGrowing(DistinctFacetCounts* a, DistinctFacetCounts* b);
  template <typename Row>
  void eachRow(Row&& row) const {
    if (window) {
      auto entries = window->entries();
      for (size_t i = 0; i < entries.size(); i++) if (entries[i]) row(window->cellKey(i), entries[i]);
    } else {
      for (size_t i = 0; i < rows.size(); i++) if (rows[i]) row(values.value(i), rows[i]);
    }
  }
  template <typename Pair>
  void eachGroup(uint64_t** row, Pair&& pair) const {
    if (!row) return;
    for (uint64_t p = 0; p < pagesPerValue; p++) if (row[p]) {
      for (uint64_t w = 0; w < pageWords(p); w++) {
        uint64_t bits = row[p][w];
        while (bits) {
          uint64_t index = (p * PAGE_WORDS + w) * 64 + std::countr_zero(bits);
          pair(groups.value(index));
          bits &= bits - 1;
        }
      }
    }
  }
  int64_t rowCount(uint64_t** row) const {
    int64_t count = 0;
    if (row) for (uint64_t i = 0; i < pagesPerValue; i++) if (row[i]) {
      for (uint64_t w = 0; w < pageWords(i); w++) count += std::popcount(row[i][w]);
    }
    return count;
  }
  static void singleton(SingletonCount& state, int64_t doc) {
    if (state.lastDoc != (uint64_t)doc) { state.count++; state.lastDoc = (uint64_t)doc; }
  }

  template <typename Group, typename Normal, typename Null, typename Single>
  static void accept(Group group, Normal&& normal, Null&& null, Single&& single) {
    if constexpr (std::is_same_v<Group, GroupIdentity>) {
      switch (group.kind) {
        case GroupIdentity::VALUE: normal(group.value); break;
        case GroupIdentity::NULL_KEY: null(); break;
        case GroupIdentity::SINGLETON: single(group.value); break;
        case GroupIdentity::DROP: break;
      }
    } else normal((int64_t)group);
  }

  template <DistinctFacetMode Mode, bool Growing = false, bool Adaptive = false, typename Visit>
  void collectAs(Visit&& visit) {
    auto normal = [&](int64_t value, int64_t group) LUXIR_INLINE {
      if constexpr (Mode == DistinctFacetMode::PAGED) {
        uint64_t index = groups.index(group);
        assert(index < groups.size);
        if constexpr (Growing) {
          if constexpr (Adaptive) if (mode == DistinctFacetMode::HASH) [[unlikely]] {
            pairs.insert({window->cellKey((size_t)value), group});
            return;
          }
          auto& row = window->entry((size_t)value);
          auto* p = row ? row[index / 64 / PAGE_WORDS] : nullptr;
          if (!p) [[unlikely]] p = growingPage(row, index / 64 / PAGE_WORDS);
          if (!p) [[unlikely]] { pairs.insert({window->cellKey((size_t)value), group}); return; }
          p[index / 64 % PAGE_WORDS] |= 1ULL << (index & 63);
        } else {
          assert(values.index(value) < values.size);
          page(rows[values.index(value)], index / 64 / PAGE_WORDS)[index / 64 % PAGE_WORDS] |= 1ULL << (index & 63);
        }
      } else if constexpr (Mode == DistinctFacetMode::HASH) pairs.insert({value, group});
      else {
        auto found = last.find(value);
        if (found == last.end()) { last.emplace(value, group); counts[value]++; }
        else if (found->second != group) { found->second = group; counts[value]++; }
      }
    };
    visit([&](int64_t value, auto group) LUXIR_INLINE {
      size_t cell = 0;
      if constexpr (Growing) cell = window->observe(value); // Before DROP filtering.
      int64_t key = value;
      if constexpr (Growing && Mode != DistinctFacetMode::PAGED) key = window->cellKey(cell);
      accept(group, [&](int64_t id) LUXIR_INLINE { normal(Growing && Mode == DistinctFacetMode::PAGED ? (int64_t)cell : key, id); },
          [&] { nullValues.insert(Growing ? window->cellKey(cell) : value); },
          [&](int64_t doc) { singleton(singletons[Growing ? window->cellKey(cell) : value], doc); });
    }, [&](auto group) LUXIR_INLINE {
      accept(group, [&](int64_t id) {
        if constexpr (Mode == DistinctFacetMode::PAGED) {
          uint64_t index = groups.index(id);
          if constexpr (Growing) {
            if (mode == DistinctFacetMode::HASH) { missingGroups.insert(id); return; }
            auto* p = missingRow ? missingRow[index / 64 / PAGE_WORDS] : nullptr;
            if (!p) p = growingPage(missingRow, index / 64 / PAGE_WORDS);
            if (p) p[index / 64 % PAGE_WORDS] |= 1ULL << (index & 63);
            else missingGroups.insert(id);
          } else page(missingRow, index / 64 / PAGE_WORDS)[index / 64 % PAGE_WORDS] |= 1ULL << (index & 63);
        } else if constexpr (Mode == DistinctFacetMode::HASH) missingGroups.insert(id);
        else if (lastMissing != id) { missingCount++; lastMissing = id; }
      }, [&] { missingNull = true; }, [&](int64_t doc) { singleton(missingSingletons, doc); });
    });
  }


public:
  Map<int64_t, int64_t> counts;

  // Keep tuple identity outside the value loop: inlining it alongside PAGED
  // counting increases register pressure in the tuple-key walk.
  LUXIR_NOINLINE GroupIdentity identity(const FacetGroupPlan& plan, const CollapseKey* key) {
    if (key == nullptr) return {GroupIdentity::DROP};
    if (key->singleton) return {GroupIdentity::SINGLETON, (int64_t)*key->singleton};
    if (!plan.internTuples) return {GroupIdentity::VALUE, plan.tupleId(*key)};
    encoded.resize(key->components.size() * 9);
    for (size_t i = 0; i < key->components.size(); i++) {
      auto part = key->components[i];
      encoded[i * 9] = (char)part.present;
      memcpy(encoded.data() + i * 9 + 1, &part.value, 8);
    }
    return {GroupIdentity::VALUE, intern(encoded)};
  }


  static bool canPage(FacetDimension values, FacetDimension groups) {
    if (!values.dense || !groups.dense || values.size == 0 || groups.size == 0) return false;
    constexpr uint64_t limit = 32ULL << 20;
    uint64_t words = groups.size / 64 + (groups.size % 64 != 0);
    if (words > limit / 8) return false;
    uint64_t rowBytes = words * 8 + (words / PAGE_WORDS + (words % PAGE_WORDS != 0) + 1) * sizeof(uint64_t*);
    return values.size < limit / rowBytes;
  }
  static DistinctFacetMode select(FacetDimension values, FacetDimension groups, bool isContiguous) {
    if (isContiguous || distinctFacetRunContractForTests) return DistinctFacetMode::RUN;
    if (forcedDistinctFacetMode == DistinctFacetMode::HASH) return DistinctFacetMode::HASH;
    bool paged = canPage(values, groups);
    if (forcedDistinctFacetMode == DistinctFacetMode::PAGED && !paged && distinctFacetStatsForTests) distinctFacetStatsForTests->refused++;
    return paged ? DistinctFacetMode::PAGED : DistinctFacetMode::HASH;
  }
  DistinctFacetCounts(RequestMemTracker& tracker, std::string_view label,
                       DistinctFacetMode mode, FacetDimension values, FacetDimension groups,
                       bool isContiguous = false)
      : resource(tracker, label), mode(mode), values(values), groups(groups),
        words(groups.size / 64 + (groups.size % 64 != 0)),
        pagesPerValue(words / PAGE_WORDS + (words % PAGE_WORDS != 0)), rows(&resource), pairs(&resource),
        nullValues(&resource), missingGroups(&resource), singletons(&resource), last(&resource), counts(&resource) {
    assert(mode != DistinctFacetMode::RUN || isContiguous || distinctFacetRunContractForTests);
    if (mode == DistinctFacetMode::PAGED) rows.assign(values.size, nullptr);
    if (distinctFacetStatsForTests) {
      if (mode == DistinctFacetMode::PAGED) distinctFacetStatsForTests->paged++;
      if (mode == DistinctFacetMode::HASH) distinctFacetStatsForTests->hash++;
      if (mode == DistinctFacetMode::RUN) distinctFacetStatsForTests->run++;
    }
  }
  // AUTO starts PAGED for a representable dense group directory, then checks
  // actual allocated state. Unknown range extents never select HASH by themselves.
  DistinctFacetCounts(RequestMemTracker& tracker, std::string_view label,
      DistinctFacetMode requested, const RangeFacetGrid& grid, FacetDimension groups,
      size_t pagedLimit = 32ULL << 20, size_t bucketCap = 100000,
      bool isContiguous = false);
  size_t residentBytes() const { return resource.bytes() + (window ? window->residentBytes() : 0); }
  DistinctFacetMode selectedMode() const { return mode; }
  size_t conversionCount() const { return conversions; }
  const RangeFacetCellWindow<uint64_t**>* cellWindow() const { return window.get(); }
  void widen(std::optional<int64_t> start, std::optional<int64_t> end) {
    assert(window);
    window->widen(start, end);
  }
  ~DistinctFacetCounts() { releasePages(); }
  // Raw values use the growing directory. Fixed collectors already supply
  // bucket identities and never instantiate this dispatch in their hot loop.
  template <typename Visit>
  void collectValues(Visit&& visit) {
    assert(window);
    switch (mode) {
      case DistinctFacetMode::PAGED:
        if (adaptive) collectAs<DistinctFacetMode::PAGED, true, true>(visit);
        else collectAs<DistinctFacetMode::PAGED, true>(visit);
        break;
      case DistinctFacetMode::HASH: collectAs<DistinctFacetMode::HASH, true>(visit); break;
      case DistinctFacetMode::RUN: collectAs<DistinctFacetMode::RUN, true>(visit); break;
      default: assert(false);
    }
  }
  template <typename Visit>
  void collect(Visit&& visit) {
    assert(!window);
    switch (mode) {
      case DistinctFacetMode::PAGED: collectAs<DistinctFacetMode::PAGED>(visit); break;
      case DistinctFacetMode::HASH: collectAs<DistinctFacetMode::HASH>(visit); break;
      case DistinctFacetMode::RUN: collectAs<DistinctFacetMode::RUN>(visit); break;
      default: assert(false);
    }
  }

  static DistinctFacetCounts* merge(DistinctFacetCounts* a, DistinctFacetCounts* b) {
    if (a->window || b->window) return mergeGrowing(a, b);
    assert(a->mode == b->mode);
    if (a->mode == DistinctFacetMode::PAGED) {
      for (uint64_t value = 0; value < a->rows.size(); value++) if (b->rows[value]) {
        for (uint64_t i = 0; i < a->pagesPerValue; i++) if (b->rows[value][i]) {
          auto* p = a->page(a->rows[value], i);
          for (uint64_t w = 0; w < a->pageWords(i); w++) p[w] |= b->rows[value][i][w];
        }
      }
      if (b->missingRow) for (uint64_t i = 0; i < a->pagesPerValue; i++) if (b->missingRow[i]) {
        auto* p = a->page(a->missingRow, i);
        for (uint64_t w = 0; w < a->pageWords(i); w++) p[w] |= b->missingRow[i][w];
      }
    } else if (a->mode == DistinctFacetMode::HASH) {
      if (!a->tuples.empty() || !b->tuples.empty()) {
        if (a->tuples.size() < b->tuples.size()) std::swap(a, b);
      } else if (a->pairs.size() < b->pairs.size()) std::swap(a, b);
      if (a->tuples.empty() && b->tuples.empty()) {
        a->pairs.insert(b->pairs.begin(), b->pairs.end());
        a->missingGroups.insert(b->missingGroups.begin(), b->missingGroups.end());
      } else {
        std::pmr::vector<int64_t> remap(b->tuples.size(), 0, &a->resource);
        for (const auto& [key, id] : b->tuples) remap[(size_t)id] = a->intern(key);
        for (auto pair : b->pairs) a->pairs.insert({pair.value, remap[(size_t)pair.group]});
        for (auto group : b->missingGroups) a->missingGroups.insert(remap[(size_t)group]);
      }
    } else {
      for (auto [value, count] : b->counts) a->counts[value] += count;
      a->missingCount += b->missingCount;
    }
    a->nullValues.insert(b->nullValues.begin(), b->nullValues.end());
    a->missingNull |= b->missingNull;
    for (auto [value, state] : b->singletons) a->singletons[value].count += state.count;
    a->missingSingletons.count += b->missingSingletons.count;
    return a;
  }

  void finish() {
    if (mode == DistinctFacetMode::PAGED) {
      eachRow([&](int64_t value, uint64_t** row) {
        int64_t count = rowCount(row);
        if (count) counts[value] += count;
      });
      missingCount += rowCount(missingRow);
    } else if (mode == DistinctFacetMode::HASH) {
      for (auto pair : pairs) counts[pair.value]++;
      missingCount += (int64_t)missingGroups.size();
    }
    for (auto value : nullValues) counts[value]++;
    for (auto [value, state] : singletons) counts[value] += state.count;
    missingCount += missingSingletons.count + missingNull;
    releasePages();
    decltype(rows) emptyRows(&resource); rows.swap(emptyRows);
    decltype(pairs) emptyPairs(&resource); pairs.swap(emptyPairs);
    decltype(nullValues) emptyNulls(&resource); nullValues.swap(emptyNulls);
    decltype(missingGroups) emptyMissing(&resource); missingGroups.swap(emptyMissing);
    decltype(singletons) emptySingletons(&resource); singletons.swap(emptySingletons);
    decltype(last) emptyLast(&resource); last.swap(emptyLast);
    decltype(tuples) emptyTuples(&resource); tuples.swap(emptyTuples);
    decltype(encoded) emptyEncoded(&resource); encoded.swap(emptyEncoded);
  }
  int64_t count(int64_t value) const { auto found = counts.find(value); return found == counts.end() ? 0 : found->second; }
  int64_t missing() const { return missingCount; }
};

} // namespace luxir
