// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

namespace luxir {

// Time threads spent blocked on storage reads that missed memory: a fetch from
// an object store, a lazily loaded file. A storage implementation that blocks
// the calling thread on such a read calls ReadWait::record with the wait, from
// that thread; a read served from memory records nothing. Searches with
// profile=true sample the calling thread's counts around each profile piece,
// and _stats reports the node's totals.
struct ReadWait {
  struct Counts {
    uint64_t reads = 0;
    uint64_t nanos = 0;
  };

  // This thread's blocking reads since it started.
  static Counts& thread() {
    thread_local Counts counts;
    return counts;
  }

  // Every thread's, for _stats.
  static inline std::atomic<uint64_t> totalReads{0};
  static inline std::atomic<uint64_t> totalNanos{0};

  // One blocking read of `nanos` on this thread.
  static void record(uint64_t nanos) {
    auto& counts = thread();
    counts.reads++;
    counts.nanos += nanos;
    totalReads.fetch_add(1, std::memory_order_relaxed);
    totalNanos.fetch_add(nanos, std::memory_order_relaxed);
  }
};

}  // namespace luxir
