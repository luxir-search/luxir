// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <deque>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <ranges>
#include <unordered_map>

#include <boost/unordered/unordered_flat_map.hpp>

#include "VectorEngine.h"
#include "luxir/search/Collapse.h"
#include "luxir/search/RequestMemTracker.h"

namespace luxir {

// Query-lifetime state. Canonical CollapsePlan keys identify groups; document
// units use a singleton (segment, doc). Neither estimates nor repeated members
// can manufacture distinct witnesses. All variable-sized state is charged.
template <bool Groups> class KnnIntervalCollector {
  struct GroupKey {
    std::optional<uint64_t> singleton;
    std::pmr::vector<CollapseKey::Component> components;
    template <typename K> GroupKey(const K& key, std::pmr::memory_resource* resource)
        : singleton(key.singleton), components(key.components.begin(), key.components.end(), resource) {}
  };
  struct Hash {
    using is_transparent = void;
    template <typename K> size_t operator()(const K& key) const { return CollapseKeyHash{}(key); }
  };
  struct Equal {
    using is_transparent = void;
    template <typename A, typename B> bool operator()(const A& a, const B& b) const {
      return a.singleton == b.singleton && std::ranges::equal(a.components, b.components);
    }
  };
  using Key = std::conditional_t<Groups, GroupKey, uint64_t>;
  using KeyView = std::conditional_t<Groups, CollapseKey, uint64_t>;
  static Key copyKey(const auto& key, std::pmr::memory_resource* resource) {
    if constexpr (Groups) return GroupKey(key, resource);
    else return key;
  }
public:
  static constexpr float NO_FLOOR = -std::numeric_limits<float>::infinity();
  static constexpr size_t NO_SLOT = std::numeric_limits<size_t>::max();
  struct Unit {
    float lower = NO_FLOOR;
    std::atomic<float> exact{NO_FLOOR};
    bool bounded = false;
    size_t heapPos = NO_SLOT;
    size_t references = 0;
    size_t seed = NO_SLOT;
  };
  struct Record {
    VectorEngineHit hit;
    float upper;
    Unit* unit;
    bool exact = false;
    bool mandatory = false;
    bool selected = false;
  };
  struct Pending {
    VectorEngineHit hit;
    float lower, upper;
    Key key;
    Pending(VectorEngineHit hit, float lower, float upper, const KeyView& key,
            std::pmr::memory_resource* resource)
        : hit(hit), lower(lower), upper(upper), key(copyKey(key, resource)) {}
  };
  using Batch = std::pmr::vector<Pending>;
private:
  struct DocumentUnits {
    std::pmr::deque<Unit> slots;
    std::pmr::vector<Unit*> free;
    explicit DocumentUnits(std::pmr::memory_resource* resource) : slots(resource), free(resource) {}
  };
  struct NoDocumentUnits {
    explicit NoDocumentUnits(std::pmr::memory_resource*) {}
  };
  using Units = std::conditional_t<Groups,
      std::pmr::unordered_map<GroupKey, Unit, Hash, Equal>,
      boost::unordered_flat_map<uint64_t, Unit*, boost::hash<uint64_t>, std::equal_to<uint64_t>,
          std::pmr::polymorphic_allocator<std::pair<const uint64_t, Unit*>>>>;
  int64_t k;
  RequestMemResource resource;
  Units units;
  // Stable slots survive flat-map rehashing; pruned units return their slots.
  [[no_unique_address]] std::conditional_t<Groups, NoDocumentUnits, DocumentUnits> documents;
  std::pmr::vector<Unit*> witnesses;
  std::pmr::vector<Record> records;
  std::mutex mutex;
  std::atomic<float> publishedFloor{NO_FLOOR};
  size_t peak = 0, nextPrune = 64;

  Unit& unitFor(const auto& key) {
    auto found = units.find(key);
    if constexpr (Groups) {
      if (found != units.end()) return found->second;
      return units.try_emplace(GroupKey(key, &resource)).first->second;
    } else {
      if (found != units.end()) return *found->second;
      Unit* unit;
      if (documents.free.empty()) unit = &documents.slots.emplace_back();
      else {
        unit = documents.free.back();
        documents.free.pop_back();
        std::destroy_at(unit);
        std::construct_at(unit);
      }
      units.emplace(key, unit);
      return *unit;
    }
  }
  void siftDown(size_t pos) {
    Unit* unit = witnesses[pos];
    size_t child;
    while ((child = pos * 2 + 1) < witnesses.size()) {
      if (child + 1 < witnesses.size() && witnesses[child + 1]->lower < witnesses[child]->lower) child++;
      if (unit->lower <= witnesses[child]->lower) break;
      witnesses[pos] = witnesses[child];
      witnesses[pos]->heapPos = pos;
      pos = child;
    }
    witnesses[pos] = unit;
    unit->heapPos = pos;
  }
  void witness(Unit& unit, float lower) {
    if (lower <= unit.lower) return;
    unit.lower = lower;
    if (unit.heapPos != NO_SLOT) siftDown(unit.heapPos);
    else if ((int64_t)witnesses.size() < k) {
      size_t pos = witnesses.size();
      witnesses.push_back(&unit);
      while (pos > 0 && witnesses[(pos - 1) / 2]->lower > lower) {
        witnesses[pos] = witnesses[(pos - 1) / 2];
        witnesses[pos]->heapPos = pos;
        pos = (pos - 1) / 2;
      }
      witnesses[pos] = &unit;
      unit.heapPos = pos;
    } else if (lower > witnesses[0]->lower) {
      witnesses[0]->heapPos = NO_SLOT;
      witnesses[0] = &unit;
      siftDown(0);
    }
    if ((int64_t)witnesses.size() == k) {
      publishedFloor.store(witnesses[0]->lower, std::memory_order_relaxed);
    }
  }
  void prune() {
    float floor = floorScore();
    std::erase_if(records, [&](const Record& record) {
      bool drop = !record.mandatory && !record.exact
          && record.upper < std::max(floor, record.unit->lower);
      if (drop) record.unit->references--;
      return drop;
    });
    auto unused = [](const Unit& unit) {
      return unit.references == 0 && unit.heapPos == NO_SLOT && !unit.bounded;
    };
    if constexpr (Groups) {
      std::erase_if(units, [&](const auto& entry) { return unused(entry.second); });
    } else {
      boost::unordered::erase_if(units, [&](const auto& entry) {
        if (!unused(*entry.second)) return false;
        documents.free.push_back(entry.second);
        return true;
      });
    }
    nextPrune = records.size() + std::max(records.size(), (size_t)64);
  }
public:
  KnnIntervalCollector(int64_t k, RequestMemTracker& tracker, std::string_view field)
      : k(k), resource(tracker, field, "knn interval state"),
        units(typename Units::allocator_type(&resource)), documents(&resource),
        witnesses(&resource), records(&resource) { assert(k > 0); }
  std::pmr::memory_resource* memoryResource() { return &resource; }
  float floorScore() const { return publishedFloor.load(std::memory_order_relaxed); }
  size_t peakRetained() const { return peak; }
  size_t unitCount() const { return units.size(); }
  std::pmr::vector<Record>& retained() { return records; }

  void offer(Batch& batch) {
    std::lock_guard lock(mutex);
    for (auto& pending : batch) {
      if (pending.upper < floorScore()) continue;
      Unit& unit = unitFor(pending.key);
      witness(unit, pending.lower);
      if (pending.upper >= std::max(floorScore(), unit.lower)) {
        records.push_back({pending.hit, pending.upper, &unit});
        unit.references++;
        peak = std::max(peak, records.size());
      }
    }
    batch.clear();
    if (records.size() >= nextPrune) prune();
  }
  // Exact slots may publish during the scan. Their results still travel on
  // the exact channel, outside either approximate collector's quota.
  void offerExact(VectorEngineHit hit, const KeyView& key) {
    std::lock_guard lock(mutex);
    Unit& unit = unitFor(key);
    unit.exact.store(std::max(unit.exact.load(std::memory_order_relaxed), hit.score), std::memory_order_relaxed);
    witness(unit, hit.score);
  }
  // Only at a round barrier or in the terminal pass.
  void resetBoundedUnits() {
    for (auto& entry : units) {
      if constexpr (Groups) entry.second.bounded = false;
      else entry.second->bounded = false;
    }
  }
  void observeBounded(const KeyView& key) { unitFor(key).bounded = true; }
  void compact() { prune(); }
  void mandatory(VectorEngineHit hit, const KeyView& key, bool exact) {
    Unit& unit = unitFor(key);
    if (exact) {
      unit.exact.store(std::max(unit.exact.load(std::memory_order_relaxed), hit.score), std::memory_order_relaxed);
      witness(unit, hit.score);
    }
    records.push_back({hit, std::numeric_limits<float>::infinity(), &unit, exact, true});
    unit.references++;
  }
};

} // namespace luxir
