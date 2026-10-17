// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <version>

namespace luxir {

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L

template <typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

#else

// libc++ versions without atomic<shared_ptr<T>> still provide the atomic
// shared_ptr free functions. They preserve ownership and publication ordering.
template <typename T>
class AtomicSharedPtr {
  std::shared_ptr<T> value;

public:
  AtomicSharedPtr() noexcept = default;
  AtomicSharedPtr(std::nullptr_t) noexcept {}
  AtomicSharedPtr(std::shared_ptr<T> desired) noexcept
      : value(std::move(desired)) {}
  AtomicSharedPtr(const AtomicSharedPtr&) = delete;
  AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

  std::shared_ptr<T> load(
      std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return std::atomic_load_explicit(&value, order);
  }

  void store(std::shared_ptr<T> desired,
             std::memory_order order = std::memory_order_seq_cst) noexcept {
    std::atomic_store_explicit(&value, std::move(desired), order);
  }

  std::shared_ptr<T> exchange(
      std::shared_ptr<T> desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept {
    return std::atomic_exchange_explicit(&value, std::move(desired), order);
  }

  bool compare_exchange_strong(std::shared_ptr<T>& expected,
                               std::shared_ptr<T> desired,
                               std::memory_order success,
                               std::memory_order failure) noexcept {
    return std::atomic_compare_exchange_strong_explicit(
        &value, &expected, std::move(desired), success, failure);
  }
};

#endif

}  // namespace luxir
