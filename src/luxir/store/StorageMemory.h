// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <functional>
#include <utility>
#include "FileIOException.h"

namespace luxir {

// One node counter, with child counters for tenants and their incarnations.
// Buffers own their charges, so unlinking a file never releases a reader's bytes.
class StorageMemory : public std::enable_shared_from_this<StorageMemory> {
  std::shared_ptr<StorageMemory> parent;
  std::atomic<uint64_t> used{0};
  uint64_t limit;
  std::function<bool()> reclaim;
  std::mutex mutex;
  std::map<std::string, std::weak_ptr<StorageMemory>, std::less<>> children;
  size_t newChildren = 0;
public:
  explicit StorageMemory(uint64_t limit = 0, std::shared_ptr<StorageMemory> parent = {}, std::function<bool()> reclaim = {})
      : parent(std::move(parent)), limit(limit), reclaim(std::move(reclaim)) {}
  // Labels are opaque to storage. The engine chooses the accounting hierarchy.
  std::shared_ptr<StorageMemory> child(std::string key) {
    std::lock_guard lock(mutex);
    auto& weak = children[std::move(key)];
    auto result = weak.lock();
    if (!result) {
      result = std::make_shared<StorageMemory>(0, shared_from_this());
      weak = result;
      if (++newChildren >= std::max((size_t)64, children.size())) {
        std::erase_if(children, [](const auto& entry) { return entry.second.expired(); });
        newChildren = 0;
      }
    }
    return result;
  }
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
        throw StorageMemoryLimitError();
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
