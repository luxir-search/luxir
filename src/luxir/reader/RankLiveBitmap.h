// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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

} // namespace luxir
