// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

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
#include "luxir/search/SearchOverrides.h"
#include "luxir/search/ops/DomainIter.h"
#include "luxir/util/screaming.h"

namespace luxir {

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

// Which of the two constructions the bucket-domain feed uses to produce the one
// domain per returned bucket.  Both end at the same DocSets; they differ in what
// they read to get there.
//
//   postings: seek each returned bucket's term and intersect its postings with
//     the incoming domain.  Reads what the returned buckets hold INDEX-WIDE,
//     plus a dictionary seek per bucket, and re-walks the domain once per
//     bucket.
//   ord column: one pass over the facet field's ord column, appending each
//     domain document to its bucket's builder.  Reads the DOMAIN, once, whatever
//     the bucket count.
//
// So the discriminator is coverage against selectivity, not either alone.  A
// bucket's index-wide docFreq is not known here without paying the seek the
// choice is about, so it is estimated from the bucket's in-domain count under
// the assumption that the filter and the facet field are uncorrelated:
//   docFreq ~ count * maxDocs / domainDocs
// A correlated filter makes that an over-estimate and can pick the column where
// postings would have won; the loss is bounded, because the column pass costs
// about what the count pass that just ran cost, while the postings side has no
// such bound - at cardinality 10 and a 1% filter it reads every posting of all
// ten returned terms, 100x the domain.
struct StrFacetBucketDomainPlan {
  // A dictionary seek decodes a term block; charged in units of the per-document
  // work the column pass does.  An order-of-magnitude charge, not a fitted one:
  // at a facet limit of 10 over 300k documents it is worth at most 0.2 of the
  // ratio below and decides no cell of the measured grid, so that grid does not
  // validate it.  It exists because the seeks are real and grow with the bucket
  // count, and MAX_BUCKETS bounds what it can claim.
  static constexpr int64_t SEEK_DOC_EQUIVALENT = 64;
  // The column must beat the postings estimate by this factor to displace it: a
  // postings block decode is streaming and vectorized, and gets cheaper still as
  // the domain densifies into bit windows, while the column pass pays an advance
  // per domain document.
  //
  // Fitted, on a 300k slice, one metric per bucket, facet limit 10, 42 cells.
  // Measured column/postings qps against the ratio this rule computes: 0.68 at
  // ratio 1.0, 0.78 at 2.0, 1.02 at 1.8, then 1.22-1.25 at 4.0-5.3, 2.8-8.9 at
  // 18-99, decaying back to ~1.0 by ratio 2,000 where the domain is small enough
  // that fixed costs are all that is left.  The one cell in [2,3) loses 1.28x
  // and the cells at [4,5.3] win 1.22-1.25x, so the crossover is just under 3.
  static constexpr int64_t MARGIN = 3;
  // One DocSet per (bucket, segment), allocated whether or not it holds
  // anything: an ArrDocSet, the separate shared_ptr control block DomainHandle
  // makes for it, their allocator headers, and the handle slot itself.
  static constexpr int64_t BYTES_PER_DOMAIN = 128;
  // Doc ids, as an upper bound across BOTH representations.  A builder promotes
  // to a bitset at maxDoc/32 documents, exactly where the array it replaces
  // would have cost maxDoc/8 bytes, and the bitset stays flat as more documents
  // land in it - so 4 bytes per domain document is never exceeded by the bitset
  // arm.  Transient vector growth slack (up to 2x on the array arm, before
  // build() shrinks to fit) is not modelled.
  static constexpr int64_t BYTES_PER_DOC = 4;
  // What one request may hold in bucket domains.  Same budget the replay bank
  // uses for its flat counters.
  static constexpr int64_t MAX_BYTES = 64 * 1024 * 1024;

  // Peak bytes the ord-column construction holds, which is also what it costs
  // OVER postings: postings materializes one bucket domain at a time and
  // discards it, so its peak is a single DocSet whatever the bucket count.
  // Split out from the rule below so a request-wide memory budget can price the
  // construction without asking whether it is the faster one.
  static __int128 bucketDomainBytes(int64_t numBuckets, int64_t numSegments,
                                    int64_t domainDocs) {
    return (__int128)numBuckets * numSegments * BYTES_PER_DOMAIN
        + (__int128)domainDocs * BYTES_PER_DOC;
  }

  static bool ordColumnBeatsPostings(int64_t domainDocs, int64_t selectedDocs,
                                     int64_t maxDocs, int64_t numBuckets,
                                     int64_t numSegments) {
    if (domainDocs <= 0 || maxDocs <= 0 || numBuckets <= 0) return false;
    // Refuse on memory before pricing speed.  The seek term below grows without
    // bound in the bucket count and would otherwise recommend the column for
    // exactly the bucket counts whose residency it cannot afford - and the
    // bucket count alone does not see it, because the table grows with the
    // segment count and the domain too.
    if (bucketDomainBytes(numBuckets, numSegments, domainDocs) > MAX_BYTES) {
      return false;
    }
    __int128 postingsRead =
        (__int128)selectedDocs * maxDocs / domainDocs
        + (__int128)numBuckets * SEEK_DOC_EQUIVALENT;
    return (__int128)domainDocs * MARGIN <= postingsRead;
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

// Visit each document in the parent domain that belongs to at least one
// selected bucket. Single-valued columns pass one owner; multi-valued columns
// group every selected owner for a document into one callback. That grouping
// is what lets child replay evaluate a document once and scatter its value.
template <typename SelectedMap, typename Callback>
void forEachSelectedBucketDoc(
    DocSet* domain, OrdColReader& parentColumn, int32_t maxDoc,
    const SelectedMap& selectedMap, Callback&& callback) {
  int64_t parentMissing = 0;
  if (!parentColumn.multiValued()) {
    forEachOrdValue(
        domain, parentColumn, maxDoc, parentMissing,
        [&](int32_t docid, int32_t storedOrd) LUXIR_INLINE {
          int32_t owner = StrFacetSelectedOrdMap::owner(
              selectedMap, storedOrd);
          if (owner >= 0) {
            callback(docid, std::span<const int32_t>(&owner, 1));
          }
        });
    return;
  }

  std::vector<int32_t> owners;
  owners.reserve(4);
  int32_t currentDoc = -1;
  auto flush = [&]() LUXIR_INLINE {
    if (!owners.empty()) callback(currentDoc, std::span<const int32_t>(owners));
    owners.clear();
  };
  forEachOrdValue(
      domain, parentColumn, maxDoc, parentMissing,
      [&](int32_t docid, int32_t storedOrd) LUXIR_INLINE {
        if (docid != currentDoc) {
          flush();
          currentDoc = docid;
        }
        int32_t owner = StrFacetSelectedOrdMap::owner(selectedMap, storedOrd);
        if (owner >= 0) owners.push_back(owner);
      });
  flush();
}

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

  static void LUXIR_INLINE increment(
      Vector& counts, int32_t owner, int64_t ord, int64_t numOrds) {
    uint64_t key = (uint64_t)owner * (uint64_t)numOrds + (uint64_t)ord;
    counts.counts[(size_t)key]++;
  }

  static void LUXIR_INLINE increment(
      Skinny& counts, int32_t owner, int64_t ord, int64_t numOrds) {
    uint64_t key = (uint64_t)owner * (uint64_t)numOrds + (uint64_t)ord;
    assert(key <= (uint64_t)std::numeric_limits<int64_t>::max());
    counts.counts.increment((int64_t)key);
  }

  static void LUXIR_INLINE increment(
      PackedHash& counts, int32_t owner, int64_t ord, int64_t numOrds) {
    uint64_t key = (uint64_t)owner * (uint64_t)numOrds + (uint64_t)ord;
    counts[key]++;
  }

  static void LUXIR_INLINE increment(
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

} // namespace luxir
