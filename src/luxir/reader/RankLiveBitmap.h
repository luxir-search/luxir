// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace luxir {

// Rank-space liveness for one physical vector column. Bits use FAISS's
// LSB-first byte layout; padding bits are zero. Sparse/multi-valued fields
// own their folded bits. Dense fields borrow LiveDocs words without owning
// LiveDocs: query handles alias its shared_ptr, which also owns this record.
struct RankLiveBitmap {
  const uint8_t* bits;
  size_t numBytes;
  int64_t liveVectors;
  std::unique_ptr<const std::vector<uint8_t>> backing;
};

// Per-IVF-list live vector counts of one ANN overlay under one LiveDocs, so
// the kNN list allocator walks a list's ids against the live bits at most
// once per delete generation instead of once per query. -1 = not counted
// yet. A count is a pure function of immutable data (the overlay's list ids
// and the live bits), so racing queries store the same value and relaxed
// atomics suffice. The overlay gen is part of the key: an overlay rebuilt on
// the same segment reassigns lists while the live bits stay shared.
struct ListLiveCounts {
  std::string field;
  uint64_t overlayGen;
  size_t numLists;
  std::unique_ptr<std::atomic<int64_t>[]> counts;
};

} // namespace luxir
