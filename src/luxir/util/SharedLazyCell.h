// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <variant>
#include "SharedLazyBuild.h"

namespace luxir {

// A non-null result is final; cacheNull also caches successful null results.
// Exceptions always retry; waiters retain the attempt's original exception.
// The published value and stored pointer are immutable. The unlocked warm read
// relies on that; in-place eviction needs atomic<shared_ptr> or a generation.
template <typename Value, bool cacheNull = true>
class SharedLazyCell {
  using Pointer = std::shared_ptr<Value>;
  std::atomic<bool> ready{false};
  std::mutex mutex;
  std::variant<Pointer, std::shared_ptr<SharedLazyBuild<Value>>> value;

public:
  SharedLazyCell() = default;
  // A cell born ready with `initial` (seeded from an already built value).
  explicit SharedLazyCell(Pointer initial) : value(std::move(initial)) {
    if (cacheNull || std::get<Pointer>(value)) ready.store(true, std::memory_order_release);
  }

  // The value when it has been built, else null; never builds or waits.
  Pointer peek() const noexcept {
    if (!ready.load(std::memory_order_acquire)) return nullptr;
    return std::get<Pointer>(value);
  }
  bool isReady() const noexcept { return ready.load(std::memory_order_acquire); }

  template <typename CreateFunc>
  Pointer getOrCreate(CreateFunc&& createFunc) {
    if (ready.load(std::memory_order_acquire)) return std::get<Pointer>(value);
    std::shared_ptr<SharedLazyBuild<Value>> pending;
    tbb::task_handle build;
    {
      std::lock_guard lock(mutex);
      if (ready.load(std::memory_order_relaxed)) return std::get<Pointer>(value);
      if (std::holds_alternative<Pointer>(value)) {
        pending = std::make_shared<SharedLazyBuild<Value>>();
        build = pending->defer([&] {
          try {
            Pointer result = std::forward<CreateFunc>(createFunc)();
            std::lock_guard lock(mutex);
            value = result;
            if (cacheNull || result) ready.store(true, std::memory_order_release);
            return result;
          } catch (...) {
            std::lock_guard lock(mutex);
            value = Pointer{};
            throw;
          }
        });
        value = pending;
      } else {
        pending = std::get<std::shared_ptr<SharedLazyBuild<Value>>>(value);
      }
    }
    return build ? pending->run(std::move(build)) : pending->wait();
  }
};

} // namespace luxir
