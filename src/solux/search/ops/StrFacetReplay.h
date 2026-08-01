#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "FacetExecution.h"
#include "SkinnyCounter.h"
#include "solux/search/SearchOverrides.h"
#include "solux/util/screaming.h"

namespace solux {

struct StrFacetColumnReplayPlan {
  static constexpr int64_t MAX_AUTO_PARENT_ORDS = 100'000;
  static constexpr int64_t MAX_AUTO_CHILD_ORDS = 100'000;
  // The grid's nominal 1% filter realizes slightly above 1%; 2% is the
  // admission margin. The next measured lane is 10% and remains excluded.
  static constexpr int64_t MAX_AUTO_DOMAIN_PERCENT = 2;
  static constexpr int64_t MAX_AUTO_OWNERS = 1'024;
  static constexpr int64_t MAX_AUTO_EXPECTED_UPDATES = 1'000'000;

  // The 2026-07-31 nested grid showed a 5.3x-12.6x replay win at or below
  // roughly 1% for parent/child dictionaries through 100K ords. The
  // 2M-parent shapes remain a near tie on the sparsest lane and strongly favor
  // selected postings/TOP_TERMS elsewhere.
  // Keep uncertain 10% and high-cardinality cases out of AUTO until the full
  // parent-count/feed planner can price them together.
  static bool dominatesBucketDomains(
      int64_t parentOrds, int64_t childOrds,
      int64_t domainDocs, int64_t maxDocs,
      int64_t owners = 1, int64_t expectedUpdates = 0) {
    return parentOrds <= MAX_AUTO_PARENT_ORDS
        && childOrds <= MAX_AUTO_CHILD_ORDS
        && owners <= MAX_AUTO_OWNERS
        && expectedUpdates <= MAX_AUTO_EXPECTED_UPDATES
        && maxDocs > 0
        && (__int128)domainDocs * 100
            <= (__int128)maxDocs * MAX_AUTO_DOMAIN_PERCENT;
  }
};

class StrFacetSelectedOrdMap {
public:
  using Dense = std::vector<int32_t>;
  using Sparse = boost::unordered_flat_map<int32_t, int32_t>;

private:
  std::variant<Dense, Sparse> map;

public:
  static constexpr int64_t DENSE_ORD_LIMIT = 65'536;

  using Selected = std::vector<std::pair<int64_t, int32_t>>;

  static Selected selectedBuckets(
      std::span<const SelectedFacetBucket<std::string_view>> buckets) {
    Selected selected;
    selected.reserve(buckets.size());
    for (const auto& bucket : buckets) {
      assert(bucket.id.has_value());
      selected.emplace_back(bucket.id->value, bucket.owner.value);
    }
    std::sort(selected.begin(), selected.end());
    return selected;
  }

  StrFacetSelectedOrdMap(
      const OrdMap::SegToGlobal& mapping,
      std::span<const SelectedFacetBucket<std::string_view>> buckets,
      StrFacetReplaySelector strategy = forcedStrFacetReplaySelector)
      : StrFacetSelectedOrdMap(
            mapping, selectedBuckets(buckets), strategy) {}

  StrFacetSelectedOrdMap(
      const OrdMap::SegToGlobal& mapping, const Selected& selected,
      StrFacetReplaySelector strategy = forcedStrFacetReplaySelector) {

    bool useDense = strategy == StrFacetReplaySelector::DENSE
        || (strategy == StrFacetReplaySelector::AUTO
            && mapping.numOrds <= DENSE_ORD_LIMIT);
    if (useDense) {
      map.emplace<Dense>((size_t)mapping.numOrds, -1);
    } else {
      auto& sparse = map.emplace<Sparse>();
      sparse.reserve(selected.size());
    }

    int64_t local = 0;
    for (const auto& [globalOrd, owner] : selected) {
      if (local >= mapping.numOrds) break;
      local = screaming::gallopLowerBound(
          local, mapping.numOrds, globalOrd,
          [&](int64_t ord) { return mapping.globalOrd(ord); });
      if (local >= mapping.numOrds) break;
      if (mapping.globalOrd(local) != globalOrd) continue;
      if (useDense) {
        std::get<Dense>(map)[(size_t)local] = owner;
      } else {
        std::get<Sparse>(map).emplace((int32_t)local, owner);
      }
      local++;
    }
  }

  bool dense() const {
    return std::holds_alternative<Dense>(map);
  }

  template <typename F>
  decltype(auto) visit(F&& visitor) {
    return std::visit(std::forward<F>(visitor), map);
  }

  static int32_t owner(const Dense& selected, int32_t storedOrd) {
    int32_t localOrd = storedOrd - 1;
    return localOrd >= 0 && localOrd < (int32_t)selected.size()
        ? selected[(size_t)localOrd] : -1;
  }

  static int32_t owner(const Sparse& selected, int32_t storedOrd) {
    auto found = selected.find(storedOrd - 1);
    return found == selected.end() ? -1 : found->second;
  }
};

// One uniform child-count representation spans all selected parent owners.
// This keeps representation dispatch outside the document loop. A packed key
// is owner * numOrds + ord; WideHash is the required fallback when that product
// cannot be represented in uint64_t.
class StrFacetReplayBank {
public:
  using OrdCount = std::pair<int64_t, int64_t>;
  using Rows = std::vector<std::vector<OrdCount>>;

  struct WideKey {
    int32_t owner;
    int64_t ord;

    bool operator==(const WideKey&) const = default;
  };

  struct WideHashFn {
    size_t operator()(const WideKey& key) const noexcept {
      uint64_t x = (uint64_t)key.ord
          ^ ((uint64_t)(uint32_t)key.owner * 0x9e3779b97f4a7c15ULL);
      x ^= x >> 30;
      x *= 0xbf58476d1ce4e5b9ULL;
      x ^= x >> 27;
      x *= 0x94d049bb133111ebULL;
      return (size_t)(x ^ (x >> 31));
    }
  };

  struct Vector {
    std::vector<int64_t> counts;
  };

  struct Skinny {
    SkinnyCounter8 counts;

    explicit Skinny(size_t slots) : counts(slots) {}
  };

  using PackedHash = boost::unordered_flat_map<uint64_t, int64_t>;
  using WideHash = boost::unordered_flat_map<WideKey, int64_t, WideHashFn>;

private:
  int32_t numOwners;
  int64_t numOrds;
  __int128 totalSlots;
  std::variant<Vector, Skinny, PackedHash, WideHash> bank;

  static constexpr size_t AUTO_FLAT_BYTE_BUDGET = 64 * 1024 * 1024;
  static constexpr size_t HASH_RESERVE_BYTE_BUDGET = 8 * 1024 * 1024;

  bool canPack() const {
    return totalSlots <= (__int128)std::numeric_limits<uint64_t>::max();
  }

  bool canAllocateVector() const {
    return totalSlots
        <= (__int128)std::vector<int64_t>().max_size();
  }

  bool canAllocateSkinny() const {
    return totalSlots <= (__int128)std::numeric_limits<int64_t>::max()
        && totalSlots <= (__int128)std::vector<uint8_t>().max_size();
  }

  bool withinAutoFlatBudget(size_t bytesPerSlot) const {
    return totalSlots * bytesPerSlot <= AUTO_FLAT_BYTE_BUDGET;
  }

public:
  StrFacetReplayBank(int32_t numOwners, int64_t numOrds,
                     int64_t expectedUpdates,
                     StrFacetReplayBankStrategy strategy =
                         forcedStrFacetReplayBank)
      : numOwners(numOwners), numOrds(numOrds),
        totalSlots((__int128)numOwners * numOrds), bank(PackedHash{}) {
    assert(numOwners >= 0 && numOrds >= 0 && expectedUpdates >= 0);

    if (strategy == StrFacetReplayBankStrategy::AUTO) {
      if (canAllocateVector() && withinAutoFlatBudget(sizeof(int64_t))
          && (__int128)expectedUpdates >= totalSlots * 16) {
        strategy = StrFacetReplayBankStrategy::VECTOR;
      } else if (!canAllocateSkinny() || !withinAutoFlatBudget(sizeof(uint8_t))
                 || totalSlots >= (__int128)expectedUpdates * 32) {
        strategy = canPack() ? StrFacetReplayBankStrategy::PACKED_HASH
                             : StrFacetReplayBankStrategy::WIDE_HASH;
      } else {
        strategy = StrFacetReplayBankStrategy::SKINNY;
      }
    }

    if ((strategy == StrFacetReplayBankStrategy::VECTOR
         || strategy == StrFacetReplayBankStrategy::SKINNY)
        && ((strategy == StrFacetReplayBankStrategy::VECTOR
                 && !canAllocateVector())
            || (strategy == StrFacetReplayBankStrategy::SKINNY
                 && !canAllocateSkinny()))) {
      strategy = StrFacetReplayBankStrategy::WIDE_HASH;
    }
    if (strategy == StrFacetReplayBankStrategy::PACKED_HASH && !canPack()) {
      strategy = StrFacetReplayBankStrategy::WIDE_HASH;
    }

    switch (strategy) {
      case StrFacetReplayBankStrategy::VECTOR:
        bank.emplace<Vector>().counts.resize((size_t)totalSlots);
        break;
      case StrFacetReplayBankStrategy::SKINNY:
        bank.emplace<Skinny>((size_t)totalSlots);
        break;
      case StrFacetReplayBankStrategy::PACKED_HASH:
        bank.emplace<PackedHash>();
        if (expectedUpdates > 0) {
          __int128 expectedDistinct =
              std::min<__int128>(expectedUpdates, totalSlots);
          auto& counts = std::get<PackedHash>(bank);
          size_t budgetEntries = HASH_RESERVE_BYTE_BUDGET
              / sizeof(PackedHash::value_type);
          size_t reserve = (size_t)std::min<__int128>(
              expectedDistinct,
              std::min(counts.max_size(), budgetEntries));
          counts.reserve(reserve);
        }
        break;
      case StrFacetReplayBankStrategy::WIDE_HASH:
        bank.emplace<WideHash>();
        if (expectedUpdates > 0) {
          __int128 expectedDistinct =
              std::min<__int128>(expectedUpdates, totalSlots);
          auto& counts = std::get<WideHash>(bank);
          size_t budgetEntries = HASH_RESERVE_BYTE_BUDGET
              / sizeof(WideHash::value_type);
          size_t reserve = (size_t)std::min<__int128>(
              expectedDistinct,
              std::min(counts.max_size(), budgetEntries));
          counts.reserve(reserve);
        }
        break;
      case StrFacetReplayBankStrategy::AUTO:
        assert(false);
        break;
    }
  }

  StrFacetReplayBankStrategy effectiveStrategy() const {
    if (std::holds_alternative<Vector>(bank)) {
      return StrFacetReplayBankStrategy::VECTOR;
    }
    if (std::holds_alternative<Skinny>(bank)) {
      return StrFacetReplayBankStrategy::SKINNY;
    }
    if (std::holds_alternative<PackedHash>(bank)) {
      return StrFacetReplayBankStrategy::PACKED_HASH;
    }
    return StrFacetReplayBankStrategy::WIDE_HASH;
  }

  static const char* strategyName(StrFacetReplayBankStrategy strategy) {
    switch (strategy) {
      case StrFacetReplayBankStrategy::AUTO: return "auto";
      case StrFacetReplayBankStrategy::VECTOR: return "vector";
      case StrFacetReplayBankStrategy::SKINNY: return "skinny";
      case StrFacetReplayBankStrategy::PACKED_HASH: return "packed-hash";
      case StrFacetReplayBankStrategy::WIDE_HASH: return "wide-hash";
    }
    return "unknown";
  }

  template <typename F>
  decltype(auto) visit(F&& visitor) {
    return std::visit(std::forward<F>(visitor), bank);
  }

  static void SOLUX_INLINE increment(
      Vector& counts, int32_t owner, int64_t ord, int64_t numOrds) {
    uint64_t key = (uint64_t)owner * (uint64_t)numOrds + (uint64_t)ord;
    counts.counts[(size_t)key]++;
  }

  static void SOLUX_INLINE increment(
      Skinny& counts, int32_t owner, int64_t ord, int64_t numOrds) {
    uint64_t key = (uint64_t)owner * (uint64_t)numOrds + (uint64_t)ord;
    assert(key <= (uint64_t)std::numeric_limits<int64_t>::max());
    counts.counts.increment((int64_t)key);
  }

  static void SOLUX_INLINE increment(
      PackedHash& counts, int32_t owner, int64_t ord, int64_t numOrds) {
    uint64_t key = (uint64_t)owner * (uint64_t)numOrds + (uint64_t)ord;
    counts[key]++;
  }

  static void SOLUX_INLINE increment(
      WideHash& counts, int32_t owner, int64_t ord, int64_t numOrds) {
    unused(numOrds);
    counts[WideKey{owner, ord}]++;
  }

  void increment(int32_t owner, int64_t ord) {
    assert(owner >= 0 && owner < numOwners);
    assert(ord >= 0 && ord < numOrds);
    visit([&](auto& counts) { increment(counts, owner, ord, numOrds); });
  }

  Rows drain() {
    Rows rows((size_t)numOwners);
    auto appendPacked = [&](uint64_t key, int64_t count) {
      int32_t owner = (int32_t)(key / (uint64_t)numOrds);
      int64_t ord = (int64_t)(key % (uint64_t)numOrds);
      rows[(size_t)owner].emplace_back(ord, count);
    };

    if (auto* counts = std::get_if<Vector>(&bank)) {
      for (size_t key = 0; key < counts->counts.size(); key++) {
        int64_t count = counts->counts[key];
        if (count != 0) appendPacked((uint64_t)key, count);
      }
    } else if (auto* counts = std::get_if<Skinny>(&bank)) {
      for (auto [key, overflow] : counts->counts.overflow) {
        int64_t count = overflow + counts->counts.counts[(size_t)key];
        counts->counts.counts[(size_t)key] = 0;
        appendPacked((uint64_t)key, count);
      }
      for (size_t key = 0; key < counts->counts.counts.size(); key++) {
        uint8_t count = counts->counts.counts[key];
        if (count != 0) appendPacked((uint64_t)key, count);
      }
    } else if (auto* counts = std::get_if<PackedHash>(&bank)) {
      for (auto [key, count] : *counts) appendPacked(key, count);
    } else {
      for (auto [key, count] : std::get<WideHash>(bank)) {
        rows[(size_t)key.owner].emplace_back(key.ord, count);
      }
    }
    return rows;
  }
};

} // namespace solux
