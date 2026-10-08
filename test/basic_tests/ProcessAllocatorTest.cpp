// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/ProcessAllocator.h"
#include "luxir/index/SortedDeletes.h"
#include "luxir/server/Stats.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace luxir;

namespace {
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
  AllocatorArena arena("test-huge-pages", true);
  if (!arena.stats()) GTEST_SKIP() << "Huge-page arena hooks require jemalloc";
  constexpr size_t blockSize = 4 * 1024 * 1024;
  void* block = arena.allocate(blockSize);
  EXPECT_EQ((uintptr_t)block % (2 * 1024 * 1024), 0);
  std::ifstream smaps("/proc/self/smaps");
  bool containing = false;
  bool advised = false;
  std::string line;
  while (std::getline(smaps, line)) {
    unsigned long begin, end;
    if (std::sscanf(line.c_str(), "%lx-%lx", &begin, &end) == 2) {
      containing = begin <= (uintptr_t)block && (uintptr_t)block < end;
    } else if (containing && line.starts_with("VmFlags:")) {
      std::istringstream flags(line);
      std::string flag;
      while (flags >> flag) if (flag == "hg") advised = true;
      break;
    }
  }
  arena.deallocate(block, blockSize);
  EXPECT_TRUE(advised);
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
  gatherStats(node, {}, response, responseMemory);
  EXPECT_EQ(response.indexing_ram.allocated_bytes, arena.stats()->allocated);
  EXPECT_GE(response.indexing_ram.resident_bytes, response.indexing_ram.allocated_bytes);
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
