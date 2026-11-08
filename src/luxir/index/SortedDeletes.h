// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <vector>
#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>

#include "luxir/index/IndexRamBudget.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/TermValHash.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

/// Entry stored in the TermValHash for the id field.
/// Each unique id has an optional local document and a delete watermark.
/// An unoccupied entry has docId -1; a zero watermark contributes no delete fact.
LUXIR_UNALIGNED_START
struct IdEntry {
  int32_t docId;
  uint64_t deleteWatermark;

  IdEntry(int32_t docId, uint64_t deleteWatermark) : docId(docId), deleteWatermark(deleteWatermark) {}

  friend std::ostream& operator<<(std::ostream& out, const IdEntry& e) {
    return out << "(doc=" << e.docId << " watermark=" << e.deleteWatermark << ")";
  }
} LUXIR_UNALIGNED_END;


/// A sorted list of (id, watermark) pairs for applying deletes to segments.
///
/// Each SortedDeletes owns a MemPool (the id string bytes) and exposes
/// sorted spans of nonzero-watermark entries. An inverter produces one span
/// combining overwrites and explicit deletes.
///
/// Lists are sorted by id, so application seeks forward through a
/// segment's id dictionary.
class SortedDeletes {
public:
  using Entry = TermValRef<IdEntry>;
  using EntrySpan = std::span<const Entry>;
  using Table = TermValHash<IdEntry>::Table;

  // Declared before storage so the last owner's destruction frees storage first.
  // Mutable so shared owners can mark a packet retirable.
  mutable IndexRamBudget::Guard ramGuard;
  // Version of the commit that flushed this packet and will retire it, or 0.
  // Passes leave the packet to that commit until it completes. Without the
  // claim, under NRT a pass would run between a commit's flushes and the
  // commit itself, writing two liveDocs generations per segment per commit. A
  // wait_for_merges commit holds its claimed packets for the merge's duration.
  uint64_t commitClaim = 0;
  // The segment flushed with this packet. It needs none of the packet: its inverter
  // resolved those ids against its own documents while indexing them.
  uint64_t sourceSegId = 0;

private:
  std::unique_ptr<MemPool> pool_;
  boost::container::small_vector<EntrySpan, 2> lists_;
  boost::container::small_vector<Table, 2> ownedTables_;
  uint64_t minVersion_ = 0;
  uint64_t maxVersion_ = 0;

public:
  SortedDeletes() = default;
  SortedDeletes(std::unique_ptr<MemPool> pool) : pool_(std::move(pool)) {}

  SortedDeletes(const SortedDeletes&) = delete;
  SortedDeletes& operator=(const SortedDeletes&) = delete;
  SortedDeletes(SortedDeletes&&) = delete;
  SortedDeletes& operator=(SortedDeletes&&) = delete;

  /// Take ownership of a sorted table, keeping its original allocation size.
  void addList(Table table, int32_t count, uint64_t minVersion, uint64_t maxVersion) {
    if (count == 0) return;
    auto* entries = table.get();
    ownedTables_.push_back(std::move(table));
    lists_.emplace_back(entries, (size_t)count);
    if (lists_.size() == 1) {
      minVersion_ = minVersion;
      maxVersion_ = maxVersion;
    } else {
      if (minVersion < minVersion_) minVersion_ = minVersion;
      if (maxVersion > maxVersion_) maxVersion_ = maxVersion;
    }
  }

  std::span<const EntrySpan> lists() const { return lists_; }

  size_t memSize() const {
    size_t bytes = sizeof(SortedDeletes) + (pool_ ? pool_->capacity() : 0);
    for (const auto& table : ownedTables_) bytes += table.get_deleter().bytes;
    return bytes;
  }

  uint64_t getLargestVersion() const { return maxVersion_; }
  uint64_t getSmallestVersion() const { return minVersion_; }

  std::string toString(size_t max = 10) const {
    std::string s = "(";
    size_t shown = 0;
    for (auto& list : lists_) {
      for (auto& e : list) {
        if (shown >= max) break;
        if (shown > 0) s += ",";
        s += (std::string_view)e;
        s += ':';
        s += std::to_string(e.val().deleteWatermark);
        shown++;
      }
    }
    s += ")";
    return s;
  }
};


/// Immutable delete batches from multiple inverters, deduplicated by identity
/// and ordered by largest version, so the batches at or below a version are a
/// prefix. Pending commits and concurrent merges share the same batches.
class MultiDeletesData {
public:
  struct ByLargestVersion {
    bool operator()(const std::shared_ptr<const SortedDeletes>& a,
                    const std::shared_ptr<const SortedDeletes>& b) const {
      if (a->getLargestVersion() != b->getLargestVersion()) return a->getLargestVersion() < b->getLargestVersion();
      return std::less<const SortedDeletes*>()(a.get(), b.get());
    }
  };
  boost::container::flat_set<std::shared_ptr<const SortedDeletes>, ByLargestVersion> deletes;

  void addAll(const MultiDeletesData& other) {
    deletes.insert(other.deletes.begin(), other.deletes.end());
  }

  void clear() { deletes.clear(); }
  size_t size() const { return deletes.size(); }

  uint64_t getLargestVersion() const {
    return deletes.empty() ? 0 : (*deletes.rbegin())->getLargestVersion();
  }

  // The batches whose largest version is in (after, through], in order.
  auto between(uint64_t after, uint64_t through) const {
    auto largest = [](const auto& batch) { return batch->getLargestVersion(); };
    auto first = std::ranges::upper_bound(deletes, after, {}, largest);
    return std::ranges::subrange(first, std::ranges::upper_bound(first, deletes.end(), through, {}, largest));
  }

  uint64_t getSmallestVersion() const {
    uint64_t smallest = std::numeric_limits<uint64_t>::max();
    for (const auto& d : deletes) {
      smallest = std::min(smallest, d->getSmallestVersion());
    }
    return smallest;
  }

  bool empty() const {
    return deletes.empty();
  }
};

} // namespace luxir
