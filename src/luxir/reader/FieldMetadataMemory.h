// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

namespace luxir {

// Live cache payload, including retained snapshots, without a reader walk.
// Bytes exclude allocator overhead and unused hash-table capacity.
struct FieldMetadataMemory {
  std::atomic<uint64_t> pagePointerArrays{0}, pagePointerBytes{0};
  std::atomic<uint64_t> pages{0}, pageBytes{0};
  std::atomic<uint64_t> entries{0}, entryBytes{0};
  std::atomic<uint64_t> openedPages{0}, openedPageBytes{0};
  std::atomic<uint64_t> openedEntries{0}, openedEntryBytes{0};
  std::atomic<uint64_t> names{0}, nameBytes{0};
  std::atomic<uint64_t> liveViews{0}, liveViewBytes{0};
  // Root live docs generations expanded from SPARSE files (reader and writer
  // copies). Not field metadata; reported as memory.live_docs_heap_bytes.
  std::atomic<uint64_t> liveDocsHeaps{0}, liveDocsHeapBytes{0};
};
inline FieldMetadataMemory fieldMetadataMemory;

} // namespace luxir
