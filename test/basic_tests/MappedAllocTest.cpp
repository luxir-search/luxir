// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "luxir/util/MappedAlloc.h"
#include "luxir/server/Stats.h"
#include "luxir/server/LuxirNode.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/mman.h>

using namespace luxir;

TEST(MappedAllocTest, ownershipAlignmentAndZeroing) {
  EXPECT_EQ(MappedAlloc(0).data(), nullptr);
  EXPECT_THROW(MappedAlloc::roundedSize(SIZE_MAX), std::length_error);
  for (int reuse = 0; reuse < 2; ++reuse) {
    MappedAlloc buffer(hugePageSize + 1);
    EXPECT_EQ(buffer.size(), 2 * hugePageSize);
    if (bigBufferArena().stats()) {
      EXPECT_EQ((uintptr_t)buffer.data() % hugePageSize, 0);
    }
    auto* ptr = (char*)buffer.data();
    EXPECT_TRUE(std::all_of(ptr, ptr + buffer.size(), [](char c) { return c == 0; }));
    std::memset(ptr, 0x55, buffer.size());
    MappedAlloc moved(std::move(buffer));
    EXPECT_EQ(buffer.data(), nullptr);
    EXPECT_EQ(buffer.size(), 0);
    buffer = MappedAlloc(1);
    buffer = std::move(moved);
    EXPECT_EQ(buffer.data(), ptr);
    EXPECT_EQ(moved.size(), 0);
    EXPECT_EQ(moved.data(), nullptr);
  }
}

TEST(MappedAllocTest, hugePageAdviceAndBackend) {
  void* address;
  {
    MappedAlloc buffer(2 * hugePageSize);
    address = buffer.data();
    std::ifstream smaps("/proc/self/smaps");
    ASSERT_TRUE(smaps);
    bool containing = false, advised = false;
    std::string line;
    while (std::getline(smaps, line)) {
      unsigned long begin, end;
      if (std::sscanf(line.c_str(), "%lx-%lx", &begin, &end) == 2) {
        containing = begin <= (uintptr_t)address && (uintptr_t)address < end;
      } else if (containing && line.starts_with("VmFlags:")) {
        std::istringstream flags(line);
        std::string flag;
        while (flags >> flag) if (flag == "hg") advised = true;
        break;
      }
    }
    EXPECT_TRUE(advised);
    LuxirNode node;
    api::StatsResponse response;
    std::pmr::monotonic_buffer_resource memory;
    gatherStats(node, {}, response, memory);
    if (auto stats = bigBufferArena().stats()) {
      EXPECT_EQ(response.big_buffer_ram.allocated_bytes, stats->allocated);
      EXPECT_GE(response.big_buffer_ram.allocated_bytes, buffer.size());
      EXPECT_GE(response.big_buffer_ram.resident_bytes, stats->allocated);
    } else {
      EXPECT_EQ(response.big_buffer_ram.allocated_bytes, 0);
      EXPECT_EQ(response.big_buffer_ram.resident_bytes, 0);
    }
  }
  if (!bigBufferArena().stats()) {
    // Sanitizer/system builds unmap immediately instead of retaining an extent.
    unsigned char resident;
    EXPECT_EQ(mincore(address, 4096, &resident), -1);
    EXPECT_EQ(errno, ENOMEM);
  }
}
