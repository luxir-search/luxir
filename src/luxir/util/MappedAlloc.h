// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

#include "luxir/util/ProcessAllocator.h"

namespace luxir {

// One full huge page can benefit from THP and arena reuse on random increments.
inline constexpr size_t mappedAllocationFloor = hugePageSize;

// Owns one zero-filled, huge-page-rounded buffer. jemalloc builds reuse a
// process arena; other builds use anonymous mappings. Advice is best effort.
class MappedAlloc {
  void* mapping = nullptr;
  size_t mappingSize = 0;

  void reset() noexcept;

public:
  MappedAlloc() = default;
  explicit MappedAlloc(size_t bytes);

  MappedAlloc(const MappedAlloc&) = delete;
  MappedAlloc& operator=(const MappedAlloc&) = delete;

  MappedAlloc(MappedAlloc&& other) noexcept
      : mapping(std::exchange(other.mapping, nullptr)),
        mappingSize(std::exchange(other.mappingSize, 0)) {}

  MappedAlloc& operator=(MappedAlloc&& other) noexcept {
    if (this != &other) {
      reset();
      mapping = std::exchange(other.mapping, nullptr);
      mappingSize = std::exchange(other.mappingSize, 0);
    }
    return *this;
  }

  ~MappedAlloc() { reset(); }

  static size_t roundedSize(size_t bytes) {
    if (bytes == 0) return 0;
    constexpr size_t MASK = hugePageSize - 1;
    if (bytes > std::numeric_limits<size_t>::max() - MASK) {
      throw std::length_error("mapped allocation size overflow");
    }
    return (bytes + MASK) & ~MASK;
  }

  void* data() { return mapping; }
  const void* data() const {
    return mapping;
  }
  size_t size() const { return mappingSize; }
};

} // namespace luxir
