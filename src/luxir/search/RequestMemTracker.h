// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>
#include <string>
#include <memory_resource>

namespace luxir {

// Request-local query-memory breaker. Charge sites name their subsystem and
// supply request-specific detail; this class owns only concurrent accounting
// and the request ceiling.
class RequestMemTracker {
  std::atomic<size_t> chargedBytes{0};
  std::atomic<size_t> peakBytes{0};
  size_t maxBytes;

  [[noreturn]] void throwLimit(std::string_view breaker,
                               std::string_view detail,
                               size_t attemptedTotal) const;

  void recordPeak(size_t total) {
    size_t peak = peakBytes.load(std::memory_order_relaxed);
    while (peak < total && !peakBytes.compare_exchange_weak(
        peak, total, std::memory_order_relaxed)) {}
  }

public:
  explicit RequestMemTracker(size_t maxBytes) : maxBytes(maxBytes) {}

  void charge(size_t bytes, std::string_view breaker,
              std::string_view detail);
  bool tryCharge(size_t bytes);
  size_t chargeUpTo(size_t preferredBytes, size_t minimumBytes,
                    std::string_view breaker, std::string_view detail);
  [[noreturn]] void chargeOverflow(std::string_view breaker,
                                   std::string_view detail) const;
  void release(size_t bytes);

  size_t bytes() const {
    return chargedBytes.load(std::memory_order_relaxed);
  }

  size_t ceiling() const { return maxBytes; }
  size_t peak() const { return peakBytes.load(std::memory_order_relaxed); }
};

// Actual allocation accounting, including container growth overlap.
class RequestMemResource : public std::pmr::memory_resource {
  RequestMemTracker& tracker;
  std::string detail;
  std::string breaker;
  std::atomic<size_t> allocatedBytes{0};

  void* do_allocate(size_t bytes, size_t alignment) override {
    tracker.charge(bytes, breaker, detail);
    try {
      void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
      allocatedBytes.fetch_add(bytes, std::memory_order_relaxed);
      return result;
    }
    catch (...) { tracker.release(bytes); throw; }
  }
  void do_deallocate(void* p, size_t bytes, size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    allocatedBytes.fetch_sub(bytes, std::memory_order_relaxed);
    tracker.release(bytes);
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }
public:
  size_t bytes() const { return allocatedBytes.load(std::memory_order_relaxed); }
  RequestMemResource(RequestMemTracker& tracker, std::string_view detail,
                     std::string_view breaker = "facet distinct state")
      : tracker(tracker), detail(detail), breaker(breaker) {}
};

} // namespace luxir
