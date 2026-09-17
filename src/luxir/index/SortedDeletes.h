// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include "luxir/util/MemPool.h"
#include "luxir/util/TermValHash.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

/// Entry stored in the TermValHash for the id field.
/// Each unique id maps to exactly one document and its version.
/// For explicit delete-by-id entries, docId is -1.
LUXIR_UNALIGNED_START
struct IdEntry {
  int32_t docId;
  uint64_t version;

  IdEntry(int32_t docId, uint64_t version) : docId(docId), version(version) {}

  friend std::ostream& operator<<(std::ostream& out, const IdEntry& e) {
    return out << "(doc=" << e.docId << " ver=" << e.version << ")";
  }
} LUXIR_UNALIGNED_END;


/// A sorted list of (id, version) pairs for applying deletes to segments.
///
/// Each SortedDeletes owns a MemPool (the id string bytes) and exposes
/// 1 or 2 sorted spans of entries:
///   - overwrite list: ids from documents indexed with overwrite=true
///   - delete-by-id list: explicit delete_ids from the update request
///
/// Both lists are sorted by id, enabling efficient k-way merge
/// when applying deletes across multiple inverters.
class SortedDeletes {
public:
  using Entry = TermValRef<IdEntry>;
  using EntrySpan = std::span<const Entry>;

private:
  std::unique_ptr<MemPool> pool_;
  boost::container::small_vector<EntrySpan, 2> lists_;
  boost::container::small_vector<char*, 2> ownedTables_;  // raw allocations to free on destruction
  uint64_t minVersion_ = 0;
  uint64_t maxVersion_ = 0;

public:
  SortedDeletes() = default;
  SortedDeletes(std::unique_ptr<MemPool> pool) : pool_(std::move(pool)) {}

  SortedDeletes(const SortedDeletes&) = delete;
  SortedDeletes& operator=(const SortedDeletes&) = delete;
  SortedDeletes(SortedDeletes&&) = default;
  SortedDeletes& operator=(SortedDeletes&&) = default;

  ~SortedDeletes() {
    for (char* p : ownedTables_) {
      delete[] p;
    }
  }

  /// Add a sorted list. The entries pointer must have been allocated with new char[]
  /// (e.g. via TermValHash::detachTable()). SortedDeletes takes ownership.
  void addList(Entry* entries, int32_t count, uint64_t minVersion, uint64_t maxVersion) {
    if (count == 0) return;
    ownedTables_.push_back(reinterpret_cast<char*>(entries));
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
        s += std::to_string(e.val().version);
        shown++;
      }
    }
    s += ")";
    return s;
  }
};


/// Immutable delete batches from multiple inverters, deduplicated by identity.
/// Pending commits and concurrent merges share the same batches.
class MultiDeletesData {
public:
  boost::unordered_flat_set<std::shared_ptr<const SortedDeletes>> deletes;

  void addAll(const MultiDeletesData& other) {
    deletes.insert(other.deletes.begin(), other.deletes.end());
  }

  void clear() { deletes.clear(); }
  size_t size() const { return deletes.size(); }

  uint64_t getLargestVersion() const {
    uint64_t largest = 0;
    for (const auto& d : deletes) {
      largest = std::max(largest, d->getLargestVersion());
    }
    return largest;
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
