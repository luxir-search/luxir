// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/LazyInputFile.h"

#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <filesystem>
#include <system_error>
#include <thread>

#include "luxir/store/FileIOException.h"
#include "luxir/util/log.h"
#include "luxir/util/thread.h"

namespace luxir {

namespace {

// A fixed set of threads that run fetches, started on first use. Fetches wait
// on the store, not the CPU, so the width bounds requests in flight rather
// than cores. Queued fetches still run when the process exits.
class FetchPool {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::function<void()>> queue;
  std::vector<std::thread> threads;
  size_t width;
  bool stopping = false;

  void work() {
    nameThisThread("luxir-fetch");
    std::unique_lock lock(mutex);
    while (true) {
      wake.wait(lock, [&] { return stopping || !queue.empty(); });
      if (queue.empty()) return;
      auto task = std::move(queue.front());
      queue.pop_front();
      lock.unlock();
      task();  // tasks catch their own errors
      task = nullptr;
      lock.lock();
    }
  }

public:
  explicit FetchPool(size_t width) : width(width) {}
  ~FetchPool() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    wake.notify_all();
    for (auto& thread : threads) thread.join();
  }

  void submit(std::function<void()> task) {
    {
      std::lock_guard lock(mutex);
      if (threads.empty()) {
        threads.reserve(width);
        for (size_t i = 0; i < width; i++) threads.emplace_back([this] { work(); });
      }
      queue.push_back(std::move(task));
    }
    wake.notify_one();
  }

  static FetchPool& shared() {
    static FetchPool pool(16);
    return pool;
  }
};

}  // namespace

LazyInputFile::LazyInputFile(std::shared_ptr<ObjectStore> store, std::string key, uint64_t size, uint64_t chunkBytes,
                             std::function<void()> onRelease)
    : store(std::move(store)), key(std::move(key)), size_(size), chunkBytes(std::max<uint64_t>(chunkBytes, 1)),
      onRelease(std::move(onRelease)) {
  chunks = (size_t)((size_ + this->chunkBytes - 1) / this->chunkBytes);
  if (size_ == 0) return;
  if (size_ > (uint64_t)SIZE_MAX / 2) throw FileIOException("object is too large to map: " + this->key);
  void* mapped = mmap(nullptr, (size_t)size_, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mapped == MAP_FAILED) {
    throw FileIOException("LazyInputFile: mmap of " + std::to_string(size_) + " bytes failed for " + this->key + ": "
                          + strerror(errno));
  }
  base = (char*)mapped;
  resident = std::make_unique<std::atomic<bool>[]>(chunks);
  for (size_t i = 0; i < chunks; i++) resident[i].store(false, std::memory_order_relaxed);
}

LazyInputFile::~LazyInputFile() {
  drain();
  if (base) munmap(base, (size_t)size_);
  if (onRelease) {
    try { onRelease(); } catch (...) {}
  }
}

void LazyInputFile::ensureChunks(size_t first, size_t last, ReadDeadline deadline) {
  // Fast path: everything is already resident.
  size_t missing = first;
  while (missing <= last && resident[missing].load(std::memory_order_acquire)) missing++;
  if (missing > last) return;

  // Lock the stripes the range covers, in ascending order so that overlapping
  // fetches cannot deadlock, then look again: another reader may have fetched.
  std::array<bool, kStripes> needed{};
  for (size_t c = first; c <= last; c++) {
    needed[c % kStripes] = true;
    if (c - first + 1 >= kStripes) break;
  }
  std::array<std::unique_lock<std::mutex>, kStripes> locks;
  for (size_t s = 0; s < kStripes; s++) {
    if (needed[s]) locks[s] = std::unique_lock(stripes[s]);
  }

  size_t c = first;
  while (c <= last) {
    if (resident[c].load(std::memory_order_acquire)) { c++; continue; }
    size_t runEnd = c;
    while (runEnd + 1 <= last && !resident[runEnd + 1].load(std::memory_order_acquire)) runEnd++;
    uint64_t begin = (uint64_t)c * chunkBytes;
    uint64_t end = std::min(size_, ((uint64_t)runEnd + 1) * chunkBytes);
    if (deadline != kNoReadDeadline && std::chrono::steady_clock::now() > deadline) {
      throw FetchTimeoutError("fetch of " + key + " missed its deadline");
    }
    try {
      store->readRange(key, begin, std::span<char>(base + begin, (size_t)(end - begin)));
    } catch (const ObjectNotFoundError& e) {
      // The bytes are gone: the reader reports the file as missing.
      throw std::filesystem::filesystem_error(e.what(), std::make_error_code(std::errc::no_such_file_or_directory));
    }
    fetchRequests_.fetch_add(1, std::memory_order_relaxed);
    fetchedBytes_.fetch_add(end - begin, std::memory_order_relaxed);
    for (size_t i = c; i <= runEnd; i++) resident[i].store(true, std::memory_order_release);
    residentChunks_.fetch_add(runEnd - c + 1, std::memory_order_relaxed);
    c = runEnd + 1;
  }
}

void LazyInputFile::ensure(uint64_t offset, uint64_t length) {
  checkRange(offset, length, size_);
  if (length == 0) return;
  ensureChunks((size_t)(offset / chunkBytes), (size_t)((offset + length - 1) / chunkBytes));
}

std::string_view LazyInputFile::read() {
  ensure(0, size_);
  return {base, (size_t)size_};
}

InputStream LazyInputFile::getInputStream() {
  ensure(0, size_);
  return InputStream(base, base + size_);
}

InputStream LazyInputFile::rangeStream(uint64_t offset, uint64_t length) {
  ensure(offset, length);
  return InputStream(base + offset, base + offset + length);
}

LazyInputFile::ChunkSpans LazyInputFile::spansOf(std::span<const ByteRange> ranges) const {
  // Merge the ranges in chunk space, so adjacent and overlapping ranges become
  // one request.
  ChunkSpans spans;
  for (const auto& range : ranges) {
    if (range.length == 0 || range.offset >= size_) continue;
    uint64_t end = std::min(size_, range.offset + std::min(range.length, size_ - range.offset));
    spans.emplace_back((size_t)(range.offset / chunkBytes), (size_t)((end - 1) / chunkBytes));
  }
  std::sort(spans.begin(), spans.end());
  // Chunks of a gap that is small enough are fetched along: one request is dearer than their bytes.
  const size_t gapChunks = (size_t)(prefetchGap / chunkBytes);
  ChunkSpans merged;
  for (size_t i = 0; i < spans.size();) {
    auto [first, last] = spans[i];
    for (i++; i < spans.size() && spans[i].first <= last + 1 + gapChunks; i++) last = std::max(last, spans[i].second);
    merged.emplace_back(first, last);
  }
  return merged;
}

bool LazyInputFile::allResident(const ChunkSpans& spans) const {
  for (auto [first, last] : spans) {
    for (size_t c = first; c <= last; c++) {
      if (!resident[c].load(std::memory_order_acquire)) return false;
    }
  }
  return true;
}

void LazyInputFile::fetchSpans(const ChunkSpans& spans, ReadDeadline deadline) {
  for (auto [first, last] : spans) ensureChunks(first, last, deadline);
}

void LazyInputFile::startFetch(ChunkSpans spans, ReadDeadline deadline, FetchDone done) {
  {
    std::lock_guard lock(inflightMutex);
    inflight++;
  }
  auto finished = [this] {
    std::lock_guard lock(inflightMutex);
    if (--inflight == 0) inflightDone.notify_all();
  };
  try {
    FetchPool::shared().submit([this, spans = std::move(spans), deadline, done, finished] {
      std::exception_ptr error;
      try {
        fetchSpans(spans, deadline);
      } catch (...) {
        error = std::current_exception();
      }
      // The file may be destroyed once this fetch no longer counts as in flight.
      finished();
      try {
        done(error);
      } catch (const std::exception& e) {
        LOG_ERROR("fetch completion failed: {}", e.what());
      } catch (...) {
        LOG_ERROR("fetch completion failed");
      }
    });
  } catch (...) {
    finished();
    done(std::current_exception());
  }
}

void LazyInputFile::fetchRanges(std::span<const ByteRange> ranges, ReadDeadline deadline, FetchDone done) {
  ChunkSpans spans;
  try {
    for (const auto& range : ranges) checkRange(range.offset, range.length, size_);
    spans = spansOf(ranges);
  } catch (...) {
    done(std::current_exception());
    return;
  }
  if (allResident(spans)) {
    done(nullptr);
    return;
  }
  startFetch(std::move(spans), deadline, std::move(done));
}

void LazyInputFile::prefetchRanges(std::span<const ByteRange> ranges) {
  auto spans = spansOf(ranges);
  if (allResident(spans)) return;
  startFetch(std::move(spans), kNoReadDeadline, [key = key](std::exception_ptr error) {
    if (!error) return;
    // Advisory: the read that needs the bytes will fetch again and report.
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& e) {
      LOG_DEBUG("prefetch of {} failed: {}", key, e.what());
    } catch (...) {}
  });
}

void LazyInputFile::prefetchRangesNow(std::span<const ByteRange> ranges) {
  try {
    fetchSpans(spansOf(ranges), kNoReadDeadline);
  } catch (const std::exception& e) {
    // Advisory: the read that needs the bytes will fetch again and report.
    LOG_DEBUG("prefetch of {} failed: {}", key, e.what());
  }
}

void LazyInputFile::drain() {
  std::unique_lock lock(inflightMutex);
  inflightDone.wait(lock, [&] { return inflight == 0; });
}

void LazyInputFile::prefetch(size_t offset, size_t length) {
  ByteRange range{offset, length};
  prefetchRanges(std::span<const ByteRange>(&range, 1));
}

}  // namespace luxir
