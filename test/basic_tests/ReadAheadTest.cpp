// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <sys/mman.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "luxir/reader/RowReadAhead.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/util/Uuid.h"

using namespace luxir;

namespace {

// A mapped file on a real filesystem (tmpfs pages cannot be dropped) whose
// pages start out of the page cache.
struct ColdFile {
  static constexpr size_t PAGE = 4096;
  std::filesystem::path path;
  int fd = -1;
  std::shared_ptr<InputFile> file;

  explicit ColdFile(size_t size) {
    auto dir = std::filesystem::exists("/var/tmp") ? std::filesystem::path("/var/tmp")
                                                   : std::filesystem::temp_directory_path();
    path = dir / ("luxir-readahead-" + newUuid());
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    std::vector<char> bytes(size, 'x');
    EXPECT_EQ((ssize_t)size, ::write(fd, bytes.data(), size));
    fsync(fd);
    file = MMapInputFile::map(fd, path);
    drop();
  }
  ~ColdFile() {
    file.reset();
    ::close(fd);
    std::filesystem::remove(path);
  }

  // Unmapped clean pages leave the cache; false when they stay (tmpfs) or
  // cannot be dropped (no posix_fadvise on macOS).
  bool drop() {
#ifdef POSIX_FADV_DONTNEED
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    return resident(0, file->size()) == 0;
#else
    return false;
#endif
  }

  size_t resident(size_t offset, size_t length) {
#if defined(__APPLE__)
    std::vector<char> vec(length / PAGE);
#else
    std::vector<unsigned char> vec(length / PAGE);
#endif
    if (mincore((void*)(file->read().data() + offset), length, vec.data()) != 0) return 0;
    size_t n = 0;
    for (auto v : vec) n += v & 1;
    return n;
  }

  bool rowResident(int64_t rank, size_t stride) {
    size_t start = (size_t)rank * stride / PAGE * PAGE;
    size_t end = ((size_t)(rank + 1) * stride + PAGE - 1) / PAGE * PAGE;
    return resident(start, end - start) == (end - start) / PAGE;
  }

  // Advice only queues the reads, so wait for them.
  bool waitResident(size_t offset, size_t length) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (resident(offset, length) < length / PAGE) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
};

} // namespace

TEST(ReadAheadTest, prefetchReadsPastTheKernelPerCallLimit) {
  ColdFile cold(4 << 20);
  if (!cold.drop()) GTEST_SKIP() << "page cache kept the test file resident";
  cold.file->prefetch(0, 2 << 20);
  EXPECT_TRUE(cold.waitResident(0, 2 << 20)) << cold.resident(0, 2 << 20) << " pages resident";
  EXPECT_EQ(0u, cold.resident(3 << 20, 1 << 20));
}

TEST(ReadAheadTest, prefetchFillsHolesBetweenResidentPages) {
  ColdFile cold(4 << 20);
  if (!cold.drop()) GTEST_SKIP() << "page cache kept the test file resident";
  for (size_t at = 0; at < (2 << 20); at += 256 * 1024) {
    cold.file->prefetch(at + 4097, 9000);
    ASSERT_TRUE(cold.waitResident(at + 4096, 3 * 4096));
  }
  cold.file->prefetch(100, (2 << 20) - 200);
  EXPECT_TRUE(cold.waitResident(0, 2 << 20)) << cold.resident(0, 2 << 20) << " pages resident";
}

namespace {

// The gate reads this thread's major faults and input blocks.
bool threadIoCounters(ColdFile& cold) {
#ifdef RUSAGE_THREAD
  struct rusage before, after;
  getrusage(RUSAGE_THREAD, &before);
  cold.file->prefetch(3 << 20, 4096);
  getrusage(RUSAGE_THREAD, &after);
  return after.ru_inblock > before.ru_inblock;
#else
  unused(cold);
  return false;
#endif
}

constexpr size_t STRIDE = 1536;

} // namespace

TEST(ReadAheadTest, probeTurnsOnForColdRowsOnly) {
  ColdFile cold(8 << 20);
  if (!cold.drop() || !threadIoCounters(cold)) GTEST_SKIP() << "no droppable pages or thread I/O counters";
  int64_t rows = (int64_t)((8 << 20) / STRIDE);
  RowReadAhead::Gate coldGate;
  RowReadAhead coldAhead(coldGate, cold.file.get(), 0, STRIDE, rows);
  coldAhead.probe({rows / 4, rows / 2});
  EXPECT_TRUE(coldAhead.active());

  cold.file->prefetch(0, 8 << 20);
  ASSERT_TRUE(cold.waitResident(0, 8 << 20));
  RowReadAhead::Gate hotGate;
  RowReadAhead hotAhead(hotGate, cold.file.get(), 0, STRIDE, rows);
  hotAhead.probe({rows / 4, rows / 2});
  EXPECT_FALSE(hotAhead.active());
}

TEST(ReadAheadTest, faultsTurnSparseAdviceOnAndResidentBatchesOff) {
  ColdFile cold(8 << 20);
  if (!cold.drop() || !threadIoCounters(cold)) GTEST_SKIP() << "no droppable pages or thread I/O counters";
  // Every other row of the first half; the second half stays hot.
  std::vector<int64_t> ranks;
  for (int64_t r = 0; r * STRIDE < (4 << 20); r += 2) ranks.push_back(r);
  cold.file->prefetch(4 << 20, 4 << 20);
  ASSERT_TRUE(cold.waitResident(4 << 20, 4 << 20));

  RowReadAhead::Gate gate;
  {
    RowReadAhead ahead(gate, cold.file.get(), 0, STRIDE, (int64_t)((8 << 20) / STRIDE));
    auto queue = [&](int64_t k) { ahead.queueRows(ranks[k], ranks[k] + 1); };
    ahead.sparseAt(0, (int64_t)ranks.size(), queue);
    EXPECT_FALSE(ahead.active());
    // Four major faults in the first 256 units: 1/64, above the 1/128 bar.
    for (size_t at : {1u << 20, 2u << 20, 3u << 20, 7u << 19}) {
      volatile char sink = cold.file->read()[at];
      unused(sink);
    }
    ahead.sparseAt(256, (int64_t)ranks.size(), queue);
    EXPECT_TRUE(ahead.active());
    for (int64_t k : {256, 500, 1000, 1279}) {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!cold.rowResident(ranks[k], STRIDE) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      EXPECT_TRUE(cold.rowResident(ranks[k], STRIDE)) << "unit " << k;
    }
  }
  // The same gate on the resident half: the first batch reads nothing.
  int64_t hotFirst = (int64_t)((4 << 20) / STRIDE) + 1;
  RowReadAhead hot(gate, cold.file.get(), 0, STRIDE, (int64_t)((8 << 20) / STRIDE));
  hot.sparseAt(0, 1000, [&](int64_t k) { hot.queueRows(hotFirst + 2 * k, hotFirst + 2 * k + 1); });
  EXPECT_FALSE(hot.active());
}

TEST(ReadAheadTest, scanAdvisesAWindowAheadOfTheCursor) {
  ColdFile cold(32 << 20);
  if (!cold.drop() || !threadIoCounters(cold)) GTEST_SKIP() << "no droppable pages or thread I/O counters";
  int64_t rows = (int64_t)((32 << 20) / STRIDE);
  RowReadAhead::Gate gate;
  RowReadAhead ahead(gate, cold.file.get(), 0, STRIDE, rows);
  ahead.scanAt(0);
  EXPECT_TRUE(ahead.active());
  EXPECT_TRUE(cold.waitResident(0, 8 << 20));
  EXPECT_LT(cold.resident(16 << 20, 4 << 20), 8u);
}

TEST(ReadAheadTest, filteredScanAdvisesOnlyEligibleRows) {
  ColdFile cold(32 << 20);
  if (!cold.drop() || !threadIoCounters(cold)) GTEST_SKIP() << "no droppable pages or thread I/O counters";
  int64_t rows = (int64_t)((32 << 20) / STRIDE);
  RowReadAhead::Gate gate;
  RowReadAhead ahead(gate, cold.file.get(), 0, STRIDE, rows);
  auto eligible = [](int64_t r) { return r % 300 == 0; };
  ahead.scanAt(0, eligible);
  EXPECT_TRUE(ahead.active());
  for (int64_t r : {0, 300, 3000, 5100}) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!cold.rowResident(r, STRIDE) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(cold.rowResident(r, STRIDE)) << "row " << r;
  }
  // The pages between rows 0 and 300 (450 KB apart) stay cold.
  EXPECT_EQ(0u, cold.resident(4096, 100 * 4096));
}
