// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "luxir/util/ProcessAllocator.h"

namespace luxir {

// Requests of at least one huge page come from bigBufferArena(), rounded up to
// whole huge pages so that all of their memory can be transparent huge pages;
// the arena keeps freed buffers for reuse until they decay. Randomly indexed
// state (counts by ordinal, dense facet entries, flat hash tables) gains from
// fewer TLB misses. Smaller requests use operator new and keep the thread
// cache: below one huge page, rounding would waste most of the page.
inline constexpr size_t bigBufferFloor = hugePageSize;

// Bytes a request of this size occupies.
inline size_t bigBufferStorage(size_t bytes) {
  if (bytes < bigBufferFloor) return bytes;
  constexpr size_t MASK = hugePageSize - 1;
  if (bytes > std::numeric_limits<size_t>::max() - MASK) {
    throw std::length_error("big buffer size overflow");
  }
  return (bytes + MASK) & ~MASK;
}

// The memory is uninitialized, as from operator new.
inline void* allocateBigBuffer(size_t bytes, size_t alignment) {
  if (bytes >= bigBufferFloor) return bigBufferArena().allocate(bigBufferStorage(bytes), alignment);
  if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) return ::operator new(bytes, std::align_val_t(alignment));
  return ::operator new(bytes);
}

inline void deallocateBigBuffer(void* ptr, size_t bytes, size_t alignment) noexcept {
  if (bytes >= bigBufferFloor) {
    bigBufferArena().deallocate(ptr, bigBufferStorage(bytes), alignment);
  } else if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    ::operator delete(ptr, bytes, std::align_val_t(alignment));
  } else {
    ::operator delete(ptr, bytes);
  }
}

template<typename T>
struct BigBufferAllocator {
  using value_type = T;

  BigBufferAllocator() = default;
  template<typename U>
  BigBufferAllocator(const BigBufferAllocator<U>&) noexcept {}

  T* allocate(size_t n) {
    if (n > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
    return (T*)allocateBigBuffer(n * sizeof(T), alignof(T));
  }

  void deallocate(T* ptr, size_t n) noexcept {
    deallocateBigBuffer(ptr, n * sizeof(T), alignof(T));
  }

  template<typename U>
  bool operator==(const BigBufferAllocator<U>&) const noexcept { return true; }
};

template<typename T>
using BigVector = std::vector<T, BigBufferAllocator<T>>;

template<typename K, typename V, typename Hash = boost::hash<K>>
using BigFlatMap = boost::unordered_flat_map<K, V, Hash, std::equal_to<K>,
                                             BigBufferAllocator<std::pair<const K, V>>>;

} // namespace luxir
