// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
    fdatasync(fd);
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
