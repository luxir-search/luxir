// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <type_traits>

namespace luxir {

template <class T, size_t ChunkShift = 9>
class ChunkedArrayView {
  const T* const* chunks = nullptr;
  size_t count = 0;

public:
  static_assert(ChunkShift < std::numeric_limits<size_t>::digits);
  static constexpr size_t CHUNK_SIZE = size_t{1} << ChunkShift;

  ChunkedArrayView() = default;
  ChunkedArrayView(const T* const* chunks, size_t count)
    : chunks(chunks), count(count) {}

  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  const T& operator[](size_t index) const {
    assert(index < count);
    return chunks[index >> ChunkShift][index & (CHUNK_SIZE - 1)];
  }

  class Iterator {
    const T* const* chunks = nullptr;
    size_t index = 0;

  public:
    using value_type = T;
    using difference_type = ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;
    using pointer = const T*;
    using reference = const T&;

    Iterator() = default;
    Iterator(const T* const* chunks, size_t index)
      : chunks(chunks), index(index) {}
    reference operator*() const {
      return chunks[index >> ChunkShift][index & (CHUNK_SIZE - 1)];
    }
    pointer operator->() const { return &**this; }
    Iterator& operator++() { ++index; return *this; }
    Iterator operator++(int) { auto prev = *this; ++*this; return prev; }
    bool operator==(const Iterator&) const = default;
  };

  Iterator begin() const { return {chunks, 0}; }
  Iterator end() const { return {chunks, count}; }
};

// Append-only storage for a monotonic resource. Only the first chunk moves;
// all allocations live until the resource is reset, with no cleanup callbacks.
// Publish views after filling; the resource must outlive every view.
template <class T, size_t ChunkShift = 9>
class ChunkedArray {
  static_assert(std::is_trivially_destructible_v<T>);
  static_assert(std::is_trivially_copyable_v<T>);

  std::pmr::memory_resource& resource;
  size_t maxSize;
  T** chunks = nullptr;
  size_t count = 0;
  size_t capacity = 0;
  size_t directoryCapacity = 0;

public:
  using View = ChunkedArrayView<T, ChunkShift>;
  static constexpr size_t CHUNK_SIZE = View::CHUNK_SIZE;

private:
  void grow() {
    if (capacity != 0 && capacity < CHUNK_SIZE) {
      size_t nextCapacity = std::min(maxSize, std::min(CHUNK_SIZE, capacity * 2));
      auto* first = (T*) resource.allocate(nextCapacity * sizeof(T), alignof(T));
      std::memcpy(first, chunks[0], count * sizeof(T));
      chunks[0] = first;
      capacity = nextCapacity;
      return;
    }

    size_t chunkIndex = count >> ChunkShift;
    if (chunkIndex == directoryCapacity) {
      size_t maxChunks = maxSize / CHUNK_SIZE + (maxSize % CHUNK_SIZE != 0);
      size_t nextCapacity = std::min(maxChunks,
          directoryCapacity == 0 ? size_t{1} : directoryCapacity * 2);
      auto** directory = (T**) resource.allocate(nextCapacity * sizeof(T*), alignof(T*));
      if (chunkIndex != 0) {
        std::memcpy(directory, chunks, chunkIndex * sizeof(T*));
      }
      chunks = directory;
      directoryCapacity = nextCapacity;
    }
    size_t chunkSize = std::min(maxSize - count,
        count == 0 ? std::min(size_t{8}, CHUNK_SIZE) : CHUNK_SIZE);
    chunks[chunkIndex] = (T*) resource.allocate(chunkSize * sizeof(T), alignof(T));
    capacity += chunkSize;
  }

public:
  ChunkedArray(std::pmr::memory_resource& resource, size_t maxSize)
    : resource(resource), maxSize(maxSize) {}
  ChunkedArray(const ChunkedArray&) = delete;
  ChunkedArray& operator=(const ChunkedArray&) = delete;

  size_t size() const { return count; }
  View view() const { return {chunks, count}; }

  // Before filling, size the directory and first chunk for a known count.
  // Remaining chunks are allocated on demand.
  void reserve(size_t expectedSize) {
    assert(capacity == 0 && directoryCapacity == 0);
    assert(expectedSize <= maxSize);
    if (expectedSize == 0) return;
    size_t numChunks = expectedSize / CHUNK_SIZE + (expectedSize % CHUNK_SIZE != 0);
    auto** directory = (T**) resource.allocate(numChunks * sizeof(T*), alignof(T*));
    size_t firstSize = std::min(expectedSize, CHUNK_SIZE);
    directory[0] = (T*) resource.allocate(firstSize * sizeof(T), alignof(T));
    chunks = directory;
    directoryCapacity = numChunks;
    capacity = firstSize;
  }

  void push_back(const T& value) {
    assert(count < maxSize);
    if (count == capacity) grow();
    std::memcpy(&chunks[count >> ChunkShift][count & (CHUNK_SIZE - 1)],
                &value, sizeof(T));
    ++count;
  }
};

} // namespace luxir
