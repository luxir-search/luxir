// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

namespace luxir {

// Reserve at admission, before handing ownership to asynchronous work. Asio
// releases its handler allocation before invoking the handler, so a completion
// can reuse the reservation for a subsequent visibility-wait delivery. Copies
// keep the storage alive even if the handler destroys its original owner.
struct HandlerStorage {
  alignas(std::max_align_t) std::byte bytes[2048];
  std::atomic<bool> used{false};
};

template<class T = std::byte>
class ReservedHandlerAllocator {
  template<class> friend class ReservedHandlerAllocator;
  std::shared_ptr<HandlerStorage> storage;
public:
  using value_type = T;
  ReservedHandlerAllocator() : storage(std::make_shared<HandlerStorage>()) {}
  template<class U>
  ReservedHandlerAllocator(const ReservedHandlerAllocator<U>& other) noexcept : storage(other.storage) {}

  T* allocate(size_t n) {
    static_assert(alignof(T) <= alignof(HandlerStorage));
    if (n > sizeof(storage->bytes) / sizeof(T) || storage->used.exchange(true)) throw std::bad_alloc();
    return (T*)storage->bytes;
  }
  void deallocate(T*, size_t) noexcept { storage->used.store(false); }
  template<class U>
  bool operator==(const ReservedHandlerAllocator<U>& other) const noexcept { return storage == other.storage; }
};

} // namespace luxir
