// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace luxir {

inline constexpr size_t STRIPES = 32;

// The calling thread's stripe, assigned round-robin on first use.  Threads past
// STRIPES share stripes: still exact, with some line sharing.
inline size_t threadStripe() {
  static std::atomic<size_t> nextStripe{0};
  thread_local size_t stripe = nextStripe.fetch_add(1, std::memory_order_relaxed) % STRIPES;
  return stripe;
}

// State updated by every thread on hot paths.  Each thread writes the block in
// its own cache-line-aligned stripe, so hot counters do not serialize on one
// shared line; readers combine the stripes.
template<class Block>
class Striped {
  struct alignas(64) Stripe {
    Block block;
  };
  std::array<Stripe, STRIPES> stripes;

public:
  Block& local() { return stripes[threadStripe()].block; }

  template<class F>
  void forEach(F&& f) const {
    for (const auto& stripe : stripes) f(stripe.block);
  }
  template<class F>
  void forEach(F&& f) {
    for (auto& stripe : stripes) f(stripe.block);
  }
};

// An exact counter bumped by every thread.
class StripedCounter {
  Striped<std::atomic<uint64_t>> stripes;

public:
  void add(uint64_t delta) {
    stripes.local().fetch_add(delta, std::memory_order_relaxed);
  }
  uint64_t load() const {
    uint64_t total = 0;
    stripes.forEach([&](const auto& value) { total += value.load(std::memory_order_relaxed); });
    return total;
  }
  void reset() {
    stripes.forEach([](auto& value) { value.store(0, std::memory_order_relaxed); });
  }
};

} // namespace luxir
