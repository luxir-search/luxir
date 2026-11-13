// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace luxir {

// Paces bytes from any number of threads to one shared rate.  Each charge
// reserves the next slot of the timeline and the caller sleeps until its slot
// ends, so concurrent writers split the rate between them.  Idle time earns no
// credit: a writer that resumes after a pause is paced from now, not allowed a
// burst.  A zero rate disables pacing.
class ByteRateLimiter {
public:
  using Clock = std::chrono::steady_clock;

private:
  uint64_t bytesPerSecond;
  std::mutex mutex;
  Clock::time_point due{};
  std::atomic<uint64_t> pausedNanos{0};

public:
  explicit ByteRateLimiter(uint64_t bytesPerSecond = 0) : bytesPerSecond(bytesPerSecond) {}

  bool enabled() const { return bytesPerSecond != 0; }
  uint64_t rate() const { return bytesPerSecond; }
  // Total time callers of pace() have slept.
  std::chrono::nanoseconds paused() const {
    return std::chrono::nanoseconds(pausedNanos.load(std::memory_order_relaxed));
  }

  // Charges `bytes` at `now` and returns when the charge has been paid off.
  Clock::time_point reserve(size_t bytes, Clock::time_point now) {
    if (!bytesPerSecond) return now;
    auto cost = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>((double)bytes / (double)bytesPerSecond));
    std::lock_guard lock(mutex);
    due = std::max(due, now) + cost;
    return due;
  }

  // Charges `bytes` and sleeps until they are paid off.
  void pace(size_t bytes) {
    if (!bytesPerSecond || bytes == 0) return;
    auto now = Clock::now();
    auto until = reserve(bytes, now);
    if (until <= now) return;
    std::this_thread::sleep_until(until);
    pausedNanos.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count(),
                          std::memory_order_relaxed);
  }
};

}
