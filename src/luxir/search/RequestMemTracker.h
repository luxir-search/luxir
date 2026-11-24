// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <string>
#include <memory_resource>

namespace luxir {

// Node-wide total of every request tracker's charges, so concurrent requests
// are bounded together. Lock-free: one CAS per charge that reaches it.
class NodeRequestMemory {
  std::atomic<size_t> chargedBytes{0};
  std::atomic<uint64_t> rejectedCount{0};
  size_t maxBytes;

public:
  explicit NodeRequestMemory(size_t maxBytes = 0) : maxBytes(maxBytes) {}
  NodeRequestMemory(const NodeRequestMemory&) = delete;
  NodeRequestMemory& operator=(const NodeRequestMemory&) = delete;

  bool tryCharge(size_t bytes) {
    size_t current = chargedBytes.load(std::memory_order_relaxed);
    for (;;) {
      if (bytes > SIZE_MAX - current || (maxBytes != 0 && current + bytes > maxBytes)) return false;
      if (chargedBytes.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) return true;
    }
  }
  void release(size_t bytes) { chargedBytes.fetch_sub(bytes, std::memory_order_relaxed); }
  // A request failed on the node-wide ceiling.
  void countRejection() { rejectedCount.fetch_add(1, std::memory_order_relaxed); }

  size_t bytes() const { return chargedBytes.load(std::memory_order_relaxed); }
  size_t ceiling() const { return maxBytes; }
  uint64_t rejected() const { return rejectedCount.load(std::memory_order_relaxed); }
};

// Request-local query-memory breaker. Charge sites name their subsystem and
// supply request-specific detail; this class owns only concurrent accounting
// and the request ceiling.
class RequestMemTracker {
  std::atomic<size_t> chargedBytes{0};
  std::atomic<size_t> peakBytes{0};
  size_t maxBytes;
  NodeRequestMemory* node;

  [[noreturn]] void throwLimit(std::string_view breaker,
                               std::string_view detail,
                               size_t attemptedTotal) const;
  [[noreturn]] void throwNodeLimit(std::string_view breaker,
                                   std::string_view detail, size_t bytes) const;

  void recordPeak(size_t total) {
    size_t peak = peakBytes.load(std::memory_order_relaxed);
    while (peak < total && !peakBytes.compare_exchange_weak(
        peak, total, std::memory_order_relaxed)) {}
  }

public:
  // Charges also count toward `node` when given; it must outlive the tracker.
  explicit RequestMemTracker(size_t maxBytes, NodeRequestMemory* node = nullptr)
      : maxBytes(maxBytes), node(node) {}

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
// RESOURCE lifetime retains charges until destruction: an arena upstream
// cannot reclaim individual allocations when its containers deallocate them.
class RequestMemResource : public std::pmr::memory_resource {
public:
  enum class Lifetime { ALLOCATION, RESOURCE };

private:
  RequestMemTracker& tracker;
  std::string detail;
  std::string breaker;
  std::atomic<size_t> allocatedBytes{0};
  std::pmr::memory_resource* upstream;
  Lifetime lifetime;

  void* do_allocate(size_t bytes, size_t alignment) override {
    tracker.charge(bytes, breaker, detail);
    try {
      void* result = upstream->allocate(bytes, alignment);
      allocatedBytes.fetch_add(bytes, std::memory_order_relaxed);
      return result;
    }
    catch (...) { tracker.release(bytes); throw; }
  }
  void do_deallocate(void* p, size_t bytes, size_t alignment) override {
    if (lifetime == Lifetime::RESOURCE) return;
    upstream->deallocate(p, bytes, alignment);
    allocatedBytes.fetch_sub(bytes, std::memory_order_relaxed);
    tracker.release(bytes);
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }
public:
  size_t bytes() const { return allocatedBytes.load(std::memory_order_relaxed); }
  RequestMemResource(RequestMemTracker& tracker, std::string_view detail,
                     std::string_view breaker = "facet distinct state",
                     std::pmr::memory_resource* upstream = std::pmr::new_delete_resource(),
                     Lifetime lifetime = Lifetime::ALLOCATION)
      : tracker(tracker), detail(detail), breaker(breaker), upstream(upstream), lifetime(lifetime) {}
  ~RequestMemResource() override { tracker.release(bytes()); }
};

} // namespace luxir
