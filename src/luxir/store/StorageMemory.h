// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <memory>
#include <functional>
#include <utility>
#include "luxir/util/ApiError.h"

namespace luxir {

// One node counter, with child counters shared by all incarnations of a collection.
// Buffers own their charges, so unlinking a file never releases a reader's bytes.
class StorageMemory {
  std::shared_ptr<StorageMemory> parent;
  std::atomic<uint64_t> used{0};
  uint64_t limit;
  std::function<bool()> reclaim;
public:
  explicit StorageMemory(uint64_t limit = 0, std::shared_ptr<StorageMemory> parent = {}, std::function<bool()> reclaim = {})
      : parent(std::move(parent)), limit(limit), reclaim(std::move(reclaim)) {}
  uint64_t bytes() const { return used.load(std::memory_order_relaxed); }
  void reserve(uint64_t bytes) {
    if (parent) parent->reserve(bytes);
    auto current = used.load(std::memory_order_relaxed);
    for (;;) {
      if (bytes > UINT64_MAX - current || (limit && bytes > limit - current)) {
        // Allocation holds no directory or reservation locks. Pressure alone
        // pays for reclamation; buffers remain charged until their last owner dies.
        if (reclaim && reclaim()) { current = used.load(std::memory_order_relaxed); continue; }
        if (parent) parent->release(bytes);
        throw ApiError(ErrorKind::RESOURCE_EXHAUSTED, "storage_memory_limit", "RAM storage memory limit exceeded");
      }
      if (used.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) return;
    }
  }
  void release(uint64_t bytes) {
    used.fetch_sub(bytes, std::memory_order_relaxed);
    if (parent) parent->release(bytes);
  }
};

class StorageCharge {
  std::shared_ptr<StorageMemory> memory;
  uint64_t bytes = 0;
public:
  StorageCharge() = default;
  StorageCharge(std::shared_ptr<StorageMemory> memory, uint64_t bytes) : memory(std::move(memory)), bytes(bytes) {
    if (this->memory) this->memory->reserve(bytes);
  }
  StorageCharge(const StorageCharge&) = delete;
  StorageCharge& operator=(const StorageCharge&) = delete;
  StorageCharge(StorageCharge&& other) noexcept : memory(std::move(other.memory)), bytes(other.bytes) {}
  StorageCharge& operator=(StorageCharge&& other) noexcept {
    if (memory) memory->release(bytes);
    memory = std::move(other.memory); bytes = other.bytes;
    return *this;
  }
  bool charged() const { return memory != nullptr; }
  ~StorageCharge() { if (memory) memory->release(bytes); }
};

}
