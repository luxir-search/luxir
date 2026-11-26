// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

namespace luxir {

// Heap bitmaps held by LiveDocs, counted as they are made and freed so stats need
// no reader walk. A generation mapped from a DENSE file is page cache and is not
// counted.
struct LiveDocsMemory {
  // Live heap bitmaps of one kind; bytes include each LiveDocs object.
  struct Counter {
    std::atomic<uint64_t> count = 0;
    std::atomic<uint64_t> bytes = 0;

    void add(uint64_t n) {
      count.fetch_add(1, std::memory_order_relaxed);
      bytes.fetch_add(n, std::memory_order_relaxed);
    }
    void remove(uint64_t n) {
      count.fetch_sub(1, std::memory_order_relaxed);
      bytes.fetch_sub(n, std::memory_order_relaxed);
    }
  };

  // Child live views derived from root deletes; reported under field_metadata.
  Counter views;
  // Root generations expanded from SPARSE files, reader and writer copies.
  Counter sparseRoots;
};
inline LiveDocsMemory liveDocsMemory;

} // namespace luxir
