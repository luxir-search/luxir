// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace luxir {

// Paces bytes from any number of threads to one shared rate.  Each charge
// reserves the next slot of the timeline and the caller waits until its slot
// ends, so concurrent writers split the rate between them.  Idle time earns no
// credit: a writer that resumes after a pause is paced from now, not allowed a
// burst.  A zero rate disables pacing.
//
// Callers run on indexing pool workers, so a wait must stay short: writers
// split their writes into chunk() pieces and charge each before writing it
// (FSFile::writeToFd), so no charge buys more than kChunkTime of the timeline.
// close() wakes every waiter and turns pacing off, so a closing node drains
// its merges at full speed instead of behind the timeline.
class ByteRateLimiter {
public:
  using Clock = std::chrono::steady_clock;
  // The share of the timeline one chunk buys.
  static constexpr std::chrono::milliseconds kChunkTime{5};
  static constexpr size_t kMinChunk = 4 << 10;
  static constexpr size_t kMaxChunk = 1 << 20;

private:
  uint64_t bytesPerSecond;
  size_t chunkBytes;
  std::mutex mutex;
  std::condition_variable closing;
  Clock::time_point due{};  // guarded by mutex
  bool closed = false;      // guarded by mutex
  std::atomic<uint64_t> pausedNanos{0};

public:
  explicit ByteRateLimiter(uint64_t bytesPerSecond = 0)
      : bytesPerSecond(bytesPerSecond),
        chunkBytes(std::clamp((size_t)(bytesPerSecond / 1000 * (uint64_t)kChunkTime.count()), kMinChunk, kMaxChunk)) {}

  bool enabled() const { return bytesPerSecond != 0; }
  uint64_t rate() const { return bytesPerSecond; }
  // The most a writer should charge at once.
  size_t chunk() const { return chunkBytes; }
  // Total time callers of pace() have waited.
  std::chrono::nanoseconds paused() const {
    return std::chrono::nanoseconds(pausedNanos.load(std::memory_order_relaxed));
  }

  // Charges `bytes` at `now` and returns when the charge has been paid off.
  Clock::time_point reserve(size_t bytes, Clock::time_point now) {
    std::lock_guard lock(mutex);
    return reserveLocked(bytes, now);
  }

  // Charges `bytes` and waits until they are paid off, or until close().
  void pace(size_t bytes) {
    if (!bytesPerSecond || bytes == 0) return;
    std::unique_lock lock(mutex);
    auto now = Clock::now();
    auto until = reserveLocked(bytes, now);
    if (until <= now) return;
    closing.wait_until(lock, until, [&] { return closed; });
    auto waited = std::min(Clock::now(), until) - now;
    pausedNanos.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count(),
                          std::memory_order_relaxed);
  }

  // Wakes every waiter; later charges return at once.
  void close() {
    {
      std::lock_guard lock(mutex);
      closed = true;
    }
    closing.notify_all();
  }

private:
  Clock::time_point reserveLocked(size_t bytes, Clock::time_point now) {
    if (!bytesPerSecond || closed) return now;
    auto cost = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>((double)bytes / (double)bytesPerSecond));
    due = std::max(due, now) + cost;
    return due;
  }
};

}
