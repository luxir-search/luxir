// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <memory_resource>
#include <utility>
#include <optional>
#include <string>

namespace luxir {

inline constexpr size_t hugePageSize = 2 * 1024 * 1024;

namespace allocator_detail {
// Return the skipped prefix and length of the whole huge pages in this range.
constexpr std::pair<size_t, size_t> hugePageInterior(uintptr_t start, size_t length) {
  size_t skip = (hugePageSize - start % hugePageSize) % hugePageSize;
  if (skip > length) return {length, 0};
  return {skip, (length - skip) & ~(hugePageSize - 1)};
}
}

struct AllocatorArenaOptions {
  bool hugePages = false;
  bool wholeHugePagePurge = false;
  // An explicit positive decay also disables eager oversize purging.
  // Background decay is best effort, not a per-arena idle deadline.
  // Unset keeps jemalloc's default decay and oversize-purge settings.
  std::optional<int64_t> dirtyDecayMs = std::nullopt;
};

struct ProcessAllocatorStats;

class AllocatorArena final : public std::pmr::memory_resource {
  std::array<unsigned, 2> arenas{};
  unsigned arenaCount = 0;
  bool hugePages;

  void* do_allocate(size_t bytes, size_t alignment) override;
  void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
  bool do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }
  void* allocateImpl(size_t bytes, size_t alignment, bool zero);
  int allocationFlags(size_t bytes, size_t alignment) const;
  void arenaControl(const char* command) noexcept;

public:
  struct Stats {
    // allocated: live size-class bytes; resident: allocator estimate including
    // metadata, active pages and dirty pages, not a measurement of process RSS.
    // Unpurged fringes of retained extents are excluded.
    size_t allocated;
    size_t resident;
  };

  explicit AllocatorArena(const char* name, AllocatorArenaOptions options = {});
  ~AllocatorArena() override;
  AllocatorArena(const AllocatorArena&) = delete;
  AllocatorArena& operator=(const AllocatorArena&) = delete;

  void* allocateZeroed(size_t bytes, size_t alignment) { return allocateImpl(bytes, alignment, true); }
  void purge() noexcept;
  // Refreshes jemalloc's statistics epoch, then reads this arena.
  std::optional<Stats> stats() const;

private:
  // Reads this arena at the current statistics epoch.
  std::optional<Stats> readStats() const;
  friend std::optional<ProcessAllocatorStats> processAllocatorStats();
};

// Configure before first use. The process-wide indexing arena is never destroyed.
void configureIndexingArena(bool hugePages);
AllocatorArena& indexingArena();
AllocatorArena& bigBufferArena();

// Process allocator, version and effective configuration for the startup banner.
std::string allocatorName();

// Current process allocator background-purging state; false without jemalloc.
bool allocatorBackgroundThreadsEnabled();

// jemalloc's process-wide statistics and the dedicated arenas, read at one
// statistics epoch; nullopt without jemalloc. allocated: live size-class bytes,
// including objects cached by threads; active: their pages; resident:
// jemalloc's upper bound on its resident pages, including metadata and dirty
// pages; retained: virtual mappings kept instead of unmapped; dirty: freed
// pages awaiting decay.
struct ProcessAllocatorStats {
  const char* version;
  size_t allocated;
  size_t active;
  size_t metadata;
  size_t resident;
  size_t mapped;
  size_t retained;
  size_t dirty;
  AllocatorArena::Stats indexing;
  AllocatorArena::Stats bigBuffer;
};
std::optional<ProcessAllocatorStats> processAllocatorStats();

// Returns free allocator memory to the OS where the allocator supports it.
void releaseFreeMemory();

// Bytes allocated by the calling thread; nullopt without jemalloc or a sanitizer.
// In sanitizer builds, first call before starting threads to install the hook.
std::optional<uint64_t> threadAllocatedBytes();

} // namespace luxir
