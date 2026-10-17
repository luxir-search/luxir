// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

namespace luxir {

// Live cache payload, including retained snapshots, without a reader walk.
// Bytes exclude allocator overhead and unused hash-table capacity.
struct FieldMetadataMemory {
  std::atomic<uint64_t> directories{0}, directoryBytes{0};
  std::atomic<uint64_t> pages{0}, pageBytes{0};
  std::atomic<uint64_t> entries{0}, entryBytes{0};
  std::atomic<uint64_t> names{0}, nameBytes{0};
};
inline FieldMetadataMemory fieldMetadataMemory;

} // namespace luxir
