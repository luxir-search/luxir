// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "ObjectStore.h"
#include "OutputStream.h"

namespace luxir {

/// An InputFile over one object of an ObjectStore that fetches byte ranges on
/// first touch.
///
/// The whole file is mapped up front as anonymous, unreserved memory, so every
/// pointer handed out is a plain pointer into one stable mapping: readers need
/// no per-access check and the "pointers stay valid as long as the file lives"
/// contract of InputFile holds. Physical memory is committed only for the
/// chunks that were fetched. A residency bitmap says which fixed-size chunks
/// were fetched; a missing range is fetched with one readRange per contiguous
/// run of missing chunks. Concurrent readers of the same chunk fetch it once:
/// the fetch holds the lock stripes of the chunks it covers, so readers of
/// unrelated ranges proceed in parallel.
///
/// Eviction is not implemented: a resident chunk stays until the file is
/// destroyed.
class LazyInputFile : public InputFile {
  static constexpr size_t kStripes = 16;

  std::shared_ptr<ObjectStore> store;
  std::string key;
  uint64_t size_;
  uint64_t chunkBytes;
  size_t chunks;
  char* base = nullptr;
  std::unique_ptr<std::atomic<bool>[]> resident;
  std::array<std::mutex, kStripes> stripes;
  std::atomic<uint64_t> fetchRequests_{0};
  std::atomic<uint64_t> fetchedBytes_{0};
  std::atomic<uint64_t> residentChunks_{0};
  std::function<void()> onRelease;
  uint64_t prefetchGap = kDefaultPrefetchGap;

  // Makes chunks [first, last] resident; one readRange per run of missing chunks.
  void ensureChunks(size_t first, size_t last);
  void ensure(uint64_t offset, uint64_t length);

public:
  /// `size` is the object's size as the store reported it. `onRelease` runs
  /// when the file is destroyed (a directory defers deleting an object that a
  /// reader still has open until then).
  LazyInputFile(std::shared_ptr<ObjectStore> store, std::string key, uint64_t size, uint64_t chunkBytes,
                std::function<void()> onRelease = {});
  ~LazyInputFile() override;
  LazyInputFile(const LazyInputFile&) = delete;
  LazyInputFile& operator=(const LazyInputFile&) = delete;

  size_t size() override { return (size_t)size_; }
  std::string_view read() override;
  InputStream getInputStream() override;
  InputStream rangeStream(uint64_t offset, uint64_t length) override;
  void prefetchRanges(std::span<const ByteRange> ranges) override;
  void prefetch(size_t offset, size_t length) override;
  bool lazy() const override { return true; }

  /// prefetchRanges fetches the chunks between two ranges too when the gap is
  /// at most this many bytes: a few extra bytes cost less than another request.
  static constexpr uint64_t kDefaultPrefetchGap = 64 << 10;
  void setPrefetchGap(uint64_t bytes) { prefetchGap = bytes; }

  const std::string& name() const { return key; }
  uint64_t chunkSize() const { return chunkBytes; }
  /// Requests made to the store, bytes fetched, and chunks resident so far.
  uint64_t fetchRequests() const { return fetchRequests_.load(std::memory_order_relaxed); }
  uint64_t fetchedBytes() const { return fetchedBytes_.load(std::memory_order_relaxed); }
  uint64_t residentChunkCount() const { return residentChunks_.load(std::memory_order_relaxed); }
};

}  // namespace luxir
