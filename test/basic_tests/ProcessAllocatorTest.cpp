// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/ProcessAllocator.h"
#include "luxir/index/SortedDeletes.h"
#include "luxir/server/Stats.h"
#include <array>
#include <chrono>
#include <thread>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/mman.h>
#include <unistd.h>

using namespace luxir;

namespace {
std::optional<bool> hugePageAdvice(void* block) {
  std::ifstream smaps("/proc/self/smaps");
  bool containing = false;
  std::string line;
  while (std::getline(smaps, line)) {
    unsigned long begin, end;
    if (std::sscanf(line.c_str(), "%lx-%lx", &begin, &end) == 2) {
      containing = begin <= (uintptr_t)block && (uintptr_t)block < end;
    } else if (containing && line.starts_with("VmFlags:")) {
      std::istringstream flags(line);
      std::string flag;
      while (flags >> flag) if (flag == "hg") return true;
      return false;
    }
  }
  return std::nullopt;
}

class CountingResource : public std::pmr::memory_resource {
  struct Allocation { size_t bytes; size_t alignment; };
  std::map<void*, Allocation> allocations;

  void* do_allocate(size_t bytes, size_t alignment) override {
    auto* ptr = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    allocations.emplace(ptr, Allocation{bytes, alignment});
    allocated += bytes;
    return ptr;
  }
  void do_deallocate(void* ptr, size_t bytes, size_t alignment) override {
    auto it = allocations.find(ptr);
    ASSERT_NE(it, allocations.end());
    EXPECT_EQ(it->second.bytes, bytes);
    EXPECT_EQ(it->second.alignment, alignment);
    allocations.erase(it);
    freed += bytes;
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
  }
  bool do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }
public:
  size_t allocated = 0;
  size_t freed = 0;
  size_t live() const { return allocated - freed; }
  ~CountingResource() override { EXPECT_TRUE(allocations.empty()); }
};
}

TEST(MemPoolUpstreamTest, blocks) {
  CountingResource upstream;
  {
    MemPool pool(&upstream);
    auto start = pool.getSavePoint();
    pool.alloc(512);
    pool.alloc(2048);
    EXPECT_GT(upstream.live(), 0);
    pool.rewind(start, 1);
    EXPECT_GT(upstream.freed, 0);
    auto freed = upstream.freed;
    pool.alloc(8192);  // Replace the retained block with a larger one.
#ifndef MEMPOOL_MALLOC
    EXPECT_GT(upstream.freed, freed);
#else
    unused(freed);
#endif
    pool.rewind(start, 0);
    EXPECT_EQ(upstream.live(), 0);
    pool.alloc(1024);  // Returned by the destructor.
  }
  EXPECT_EQ(upstream.live(), 0);
}

TEST(TermValHash, upstreamTablesAndDetachedOwnership) {
  CountingResource upstream;
  {
    auto pool = std::make_unique<MemPool>(&upstream);
    TermValHash<IdEntry> hash(*pool, 4);
    EXPECT_EQ(upstream.live(), 4 * sizeof(TermValRef<IdEntry>));
    for (int i = 0; i < 100; ++i) {
      auto key = std::to_string(i);
      hash.try_emplace(std::string_view(key), i, (uint64_t)i);
    }
    EXPECT_GT(upstream.freed, 0);  // Old tables were returned during rehash.
    auto* entries = hash.destructiveCompress();
    std::sort(entries, entries + hash.size());
    SortedDeletes deletes(std::move(pool));
    deletes.addList(hash.detachTable(), (int32_t)hash.size(), 0, 99);
    ASSERT_EQ(deletes.lists().size(), 1);
    EXPECT_EQ(deletes.lists()[0].size(), 100);
  }
  EXPECT_EQ(upstream.live(), 0);
}

TEST(TermValHash, tableReleaseAfterPoolDestruction) {
  CountingResource upstream;
  auto pool = std::make_unique<MemPool>(&upstream);
  TermValHash<IdEntry> hash(*pool, 4);
  hash.try_emplace(std::string_view("deleted"), 0, (uint64_t)1);
  {
    SortedDeletes deletes(std::move(pool));
    // An exception before detaching the hash leaves its table owned by the hash.
  }
  EXPECT_GT(upstream.live(), 0);
  hash.free();
  EXPECT_EQ(upstream.live(), 0);
}

TEST(ProcessAllocatorTest, arenaReclaimsBlocks) {
  AllocatorArena arena("test-indexing");
  auto start = arena.stats();
  if (!start) GTEST_SKIP() << "Arena statistics require jemalloc with stats enabled";
  constexpr size_t blockSize = 4 * 1024 * 1024;
  std::array<void*, 4> blocks;
  for (auto& block : blocks) {
    block = arena.allocate(blockSize);
    std::memset(block, 1, blockSize);
  }
  auto live = *arena.stats();
  EXPECT_GE(live.allocated, start->allocated + blocks.size() * blockSize);
  for (auto block : blocks) arena.deallocate(block, blockSize);
  EXPECT_EQ(arena.stats()->allocated, start->allocated);
  arena.purge();
  EXPECT_LT(arena.stats()->resident, live.resident);
  EXPECT_LT(arena.stats()->resident, start->resident + blockSize);
}

TEST(ProcessAllocatorTest, hugePageAdvice) {
  AllocatorArena arena("test-huge-pages", {.hugePages = true});
  if (!arena.stats()) GTEST_SKIP() << "Huge-page arena hooks require jemalloc";
  constexpr size_t blockSize = 4 * 1024 * 1024;
  void* block = arena.allocate(blockSize);
  EXPECT_EQ((uintptr_t)block % (2 * 1024 * 1024), 0);
  auto advised = hugePageAdvice(block);
  arena.deallocate(block, blockSize);
  EXPECT_EQ(advised, std::optional<bool>(true));
}

TEST(ProcessAllocatorTest, hugePageAllocationRouting) {
  for (bool wholePurge : {false, true}) {
    AllocatorArena arena("test-routing", {.hugePages = true, .wholeHugePagePurge = wholePurge});
    bool jemalloc = arena.stats().has_value();
    for (size_t size : {size_t{0}, size_t{1024}, hugePageSize - 1, hugePageSize,
                        hugePageSize + 1, hugePageSize * 3 / 2, hugePageSize * 2}) {
      for (size_t alignment : {alignof(std::max_align_t), 2 * hugePageSize}) {
        SCOPED_TRACE(::testing::Message() << "size=" << size << " alignment=" << alignment
                                        << " wholePurge=" << wholePurge);
        void* block = arena.allocateZeroed(size, alignment);
        EXPECT_EQ((uintptr_t)block % alignment, 0);
        EXPECT_TRUE(std::all_of((char*)block, (char*)block + size, [](char c) { return c == 0; }));
        if (jemalloc) {
          bool huge = size != 0 && size % hugePageSize == 0;
          EXPECT_EQ(hugePageAdvice(block), std::optional<bool>(huge));
          if (huge) {
            EXPECT_EQ((uintptr_t)block % hugePageSize, 0);
          }
        }
        std::memset(block, 1, size);
        arena.deallocate(block, size, alignment);
      }
    }
    if (jemalloc) {
      EXPECT_EQ(arena.stats()->allocated, 0);
    }
    arena.purge();
  }
}

TEST(ProcessAllocatorTest, companionStatsAndPurge) {
  AllocatorArena arena("test-companion", {.hugePages = true, .dirtyDecayMs = 60000});
  auto start = arena.stats();
  if (!start) GTEST_SKIP() << "Arena statistics require jemalloc with stats enabled";
  constexpr size_t hugeSize = 4 * hugePageSize;
  constexpr size_t regularSize = hugePageSize / 2;
  void* huge = arena.allocate(hugeSize);
  std::memset(huge, 1, hugeSize);
  std::array<void*, 8> regular;
  for (auto& block : regular) {
    block = arena.allocate(regularSize);
    std::memset(block, 2, regularSize);
  }
  auto live = *arena.stats();
  EXPECT_EQ(live.allocated, start->allocated + hugeSize + regular.size() * regularSize);
  EXPECT_GE(live.resident, start->resident + hugeSize + regular.size() * regularSize);
  arena.deallocate(huge, hugeSize);
  arena.purge();
  auto regularLive = *arena.stats();
  EXPECT_EQ(regularLive.allocated, start->allocated + regular.size() * regularSize);
  EXPECT_LT(regularLive.resident, live.resident - hugeSize + hugePageSize);
  for (auto block : regular) {
    EXPECT_TRUE(std::all_of((char*)block, (char*)block + regularSize, [](char c) { return c == 2; }));
    arena.deallocate(block, regularSize);
  }
  arena.purge();
  auto purged = *arena.stats();
  EXPECT_EQ(purged.allocated, start->allocated);
  EXPECT_LT(purged.resident, regularLive.resident - regular.size() * regularSize + hugePageSize);
  EXPECT_LT(purged.resident, start->resident + hugePageSize);
}

TEST(ProcessAllocatorTest, inverterFlushReclaimsArena) {
  auto& arena = indexingArena();
  if (!arena.stats()) GTEST_SKIP() << "Arena statistics require jemalloc with stats enabled";
  LuxirNode node;
  test::CollectionHelper helper(node);
  arena.purge();
  auto start = *arena.stats();
  auto& writer = *helper.getIndexWriter();
  auto& inverter = writer.obtainInverter();
  auto& text = inverter.getIndexHandler("text_w");
  for (int i = 0; i < 20000; ++i) {
    inverter.startDoc();
    auto value = "word" + std::to_string(i) + " common words";
    text.index(inverter, value);
    inverter.finishDoc();
  }
  EXPECT_GT(arena.stats()->allocated, start.allocated + 1024 * 1024);
  api::StatsResponse response;
  std::pmr::monotonic_buffer_resource responseMemory;
  gatherStats(node, {.memory = true}, response, responseMemory);
  ASSERT_TRUE(response.memory.has_value() && response.memory->jemalloc.has_value());
  const auto& jemalloc = *response.memory->jemalloc;
  EXPECT_EQ(jemalloc.indexing.allocated_bytes, arena.stats()->allocated);
  EXPECT_GE(jemalloc.indexing.resident_bytes, jemalloc.indexing.allocated_bytes);
  // The indexing arena is part of the process-wide totals.
  EXPECT_GE(jemalloc.allocated_bytes, jemalloc.indexing.allocated_bytes);
  EXPECT_LE(jemalloc.allocated_bytes, jemalloc.active_bytes);
  EXPECT_LE(jemalloc.active_bytes, jemalloc.resident_bytes);
  EXPECT_LE(jemalloc.active_bytes, jemalloc.mapped_bytes);
  EXPECT_FALSE(jemalloc.version.empty());
  writer.releaseInverter(inverter, true);
  writer.updateGraph.wait_for_all();
  auto flushed = *arena.stats();  // No test-side purge: the flush must do it.
  EXPECT_EQ(flushed.allocated, start.allocated);
  EXPECT_LT(flushed.resident, start.resident + 4 * 1024 * 1024);
}

TEST(RAMFileTest, upstreamSurvivesBufferTransfer) {
  CountingResource upstream;
  RAMFile destination("destination");
  {
    RAMFile source("source", {}, &upstream);
    OutputStream out(&source);
    out.write("value", 5);
    out.close();
    EXPECT_GT(upstream.live(), 0);
    destination.destructiveAppend(source);
  }
  EXPECT_GT(upstream.live(), 0);
  char value[5];
  destination.copyTo(value);
  EXPECT_EQ(std::string_view(value, 5), "value");
  destination.clear();
  EXPECT_EQ(upstream.live(), 0);
}

TEST(ProcessAllocatorTest, alignedAllocation) {
  AllocatorArena arena("test-indexing");
  constexpr size_t alignment = 256;
  void* block = arena.allocate(1234, alignment);
  EXPECT_EQ((uintptr_t)block % alignment, 0);
  std::memset(block, 1, 1234);
  arena.deallocate(block, 1234, alignment);
  arena.purge();
}

TEST(ProcessAllocatorTest, abortedFlushAndDestructionReclaimArena) {
  auto& arena = indexingArena();
  if (!arena.stats()) GTEST_SKIP() << "Arena statistics require jemalloc with stats enabled";
  for (bool abortFlush : {true, false}) {
    arena.purge();
    auto start = *arena.stats();
    {
      LuxirNode node;
      test::CollectionHelper helper(node);
      auto& writer = *helper.getIndexWriter();
      auto& inverter = writer.obtainInverter();
      inverter.deleteId("deleted", 1);
      auto* bytes = inverter.pool.alloc(MemPool::BYTE_BLOCK_SIZE - MemPool::HEADER_SIZE);
      std::memset(bytes, 1, MemPool::BYTE_BLOCK_SIZE - MemPool::HEADER_SIZE);
      EXPECT_GT(arena.stats()->allocated, start.allocated + 1024 * 1024);
      if (abortFlush) {
        inverter.fail(std::make_exception_ptr(std::runtime_error("aborted indexing")));
        writer.releaseInverter(inverter, true);
        writer.updateGraph.wait_for_all();
        EXPECT_EQ(arena.stats()->allocated, start.allocated);
        EXPECT_LT(arena.stats()->resident, start.resident + 4 * 1024 * 1024);
      } else {
        writer.releaseInverter(inverter);
        writer.close();
        EXPECT_GT(arena.stats()->allocated, start.allocated + 1024 * 1024);
      }
    }
    auto freed = *arena.stats();
    EXPECT_EQ(freed.allocated, start.allocated);
    EXPECT_LT(freed.resident, start.resident + 4 * 1024 * 1024);
  }
}

TEST(ProcessAllocatorTest, purgePreservesLiveAllocations) {
  AllocatorArena arena("test-indexing");
  AllocatorArena other("test-query");
  if (!arena.stats()) GTEST_SKIP() << "Arena statistics require jemalloc with stats enabled";
  constexpr size_t blockSize = 4 * 1024 * 1024;
  auto* live = (char*)arena.allocate(blockSize);
  auto* warm = (char*)other.allocate(blockSize);
  void* freed = arena.allocate(blockSize);
  std::memset(live, 17, blockSize);
  std::memset(warm, 23, blockSize);
  std::memset(freed, 42, blockSize);
  auto otherStats = *other.stats();
  arena.deallocate(freed, blockSize);
  arena.purge();
  EXPECT_EQ(arena.stats()->allocated, blockSize);
  EXPECT_EQ(other.stats()->resident, otherStats.resident);
  EXPECT_TRUE(std::all_of(live, live + blockSize, [](char v) { return v == 17; }));
  EXPECT_TRUE(std::all_of(warm, warm + blockSize, [](char v) { return v == 23; }));
  arena.deallocate(live, blockSize);
  other.deallocate(warm, blockSize);
}

TEST(ProcessAllocatorTest, wholeHugePageInterior) {
  constexpr size_t H = hugePageSize;
  for (size_t start : {0u, 4096u, 2u * 1024 * 1024 - 4096u}) {
    for (size_t length : {size_t{0}, size_t{4096}, H - 4096, H, H + 4096, 2 * H + 4096}) {
      auto [skip, interior] = allocator_detail::hugePageInterior(start, length);
      EXPECT_LE(skip + interior, length);
      EXPECT_EQ(interior % H, 0);
      if (interior) {
        EXPECT_EQ((start + skip) % H, 0);
      }
      // Every complete huge page, and no partial page, must be included.
      for (size_t page = 0; page <= start + length; page += H) {
        bool whole = page >= start && page + H <= start + length;
        bool purged = page >= start + skip && page + H <= start + skip + interior;
        EXPECT_EQ(whole, purged);
      }
    }
  }
}

TEST(ProcessAllocatorTest, wholeHugePagePurgeAndReuse) {
  AllocatorArena arena("test-big-buffers",
      {.wholeHugePagePurge = true, .dirtyDecayMs = 60000});
  if (!arena.stats()) GTEST_SKIP() << "Arena hooks require jemalloc";
  constexpr size_t size = 8 * hugePageSize; // Above the default eager-purge threshold.
  void* first = arena.allocateZeroed(size, hugePageSize);
  void* live = arena.allocateZeroed(size, hugePageSize);
  std::memset(first, 0x55, size);
  std::memset(live, 0x33, size);
  auto resident = arena.stats()->resident;
  arena.deallocate(first, size, hugePageSize);
  EXPECT_GE(arena.stats()->resident, resident);
  void* reused = arena.allocateZeroed(size, hugePageSize);
  EXPECT_EQ(reused, first);
  EXPECT_TRUE(std::all_of((char*)reused, (char*)reused + size, [](char c) { return c == 0; }));
  std::memset(reused, 0x55, size);
  arena.deallocate(reused, size, hugePageSize);
  arena.purge();
  EXPECT_LT(arena.stats()->resident, resident);
  std::vector<unsigned char> pages(size / (size_t)sysconf(_SC_PAGESIZE));
  EXPECT_EQ(mincore(reused, size, pages.data()), 0);
  EXPECT_TRUE(std::all_of(pages.begin(), pages.end(), [](auto p) { return !(p & 1); }));
  EXPECT_EQ(mincore(live, size, pages.data()), 0);
  EXPECT_TRUE(std::all_of(pages.begin(), pages.end(), [](auto p) { return p & 1; }));
  EXPECT_TRUE(std::all_of((char*)live, (char*)live + size, [](char c) { return c == 0x33; }));
  // Retained extents that were only partially purged must also be re-zeroed.
  reused = arena.allocateZeroed(size, hugePageSize);
  EXPECT_TRUE(std::all_of((char*)reused, (char*)reused + size, [](char c) { return c == 0; }));
  arena.deallocate(reused, size, hugePageSize);
  arena.deallocate(live, size, hugePageSize);
  arena.purge();
}

TEST(ProcessAllocatorTest, backgroundDecayAndReuse) {
  AllocatorArena arena("test-background-decay",
      {.wholeHugePagePurge = true, .dirtyDecayMs = 250});
  auto start = arena.stats();
  if (!start) GTEST_SKIP() << "Arena decay requires jemalloc with stats enabled";
  if (!allocatorBackgroundThreadsEnabled()) {
    GTEST_SKIP() << "Background decay requires background_thread:true (MALLOC_CONF overrides it)";
  }
  constexpr size_t size = 8 * hugePageSize;
  constexpr size_t regularSize = size + 4096;
  for (int burst = 0; burst < 2; ++burst) {
    SCOPED_TRACE(burst);
    std::array<void*, 4> huge, regular;
    for (size_t i = 0; i < huge.size(); ++i) {
      huge[i] = arena.allocateZeroed(size, hugePageSize);
      regular[i] = arena.allocateZeroed(regularSize, alignof(std::max_align_t));
      std::memset(huge[i], 1, size);
      std::memset(regular[i], 1, regularSize);
    }
    // Both arenas exceed the 1024-page wake threshold. Spread frees across
    // epochs so a missed trylock has further wake opportunities, including
    // after a worker's 100 ms minimum sleep. Then observe with no arena work.
    for (size_t i = 0; i < huge.size(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(110));
      arena.deallocate(huge[i], size, hugePageSize);
      arena.deallocate(regular[i], regularSize);
    }
    EXPECT_EQ(arena.stats()->allocated, start->allocated);
    // Allow multiple decay windows and scheduling delay, not a hard deadline.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (arena.stats()->resident >= start->resident + hugePageSize &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_LT(arena.stats()->resident, start->resident + hugePageSize);
  }
}
