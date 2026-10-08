// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>

namespace luxir {

class AllocatorArena final : public std::pmr::memory_resource {
  unsigned arena = 0;
  bool hugePages;

  void* do_allocate(size_t bytes, size_t alignment) override;
  void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
  bool do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }
  int allocationFlags(size_t bytes, size_t alignment) const;

public:
  struct Stats {
    // allocated: live size-class bytes; resident: allocator estimate including
    // metadata, active pages and dirty pages, not a measurement of process RSS.
    size_t allocated;
    size_t resident;
  };

  explicit AllocatorArena(const char* name, bool hugePages = false);
  ~AllocatorArena() override;
  AllocatorArena(const AllocatorArena&) = delete;
  AllocatorArena& operator=(const AllocatorArena&) = delete;

  void purge() noexcept;
  std::optional<Stats> stats() const;
};

// Configure before first use. The process-wide indexing arena is never destroyed.
void configureIndexingArena(bool hugePages);
AllocatorArena& indexingArena();

// Process allocator, version and effective configuration for the startup banner.
std::string allocatorName();

// Returns free allocator memory to the OS where the allocator supports it.
void releaseFreeMemory();

// Bytes allocated by the calling thread; nullopt without jemalloc or a sanitizer.
// In sanitizer builds, first call before starting threads to install the hook.
std::optional<uint64_t> threadAllocatedBytes();

} // namespace luxir
