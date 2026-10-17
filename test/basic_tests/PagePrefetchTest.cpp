// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "luxir/util/PagePrefetch.h"

using namespace luxir;

namespace {

// A private file mapping whose pages are dropped from the page cache.
struct ColdMapping {
  size_t size = 64 * 4096;
  char* data = nullptr;
  int fd = -1;

  ColdMapping() {
    char path[] = "/tmp/luxir_prefetch_XXXXXX";
    fd = mkstemp(path);
    unlink(path);
    std::vector<char> bytes(size, 'x');
    EXPECT_EQ((ssize_t)size, ::write(fd, bytes.data(), size));
    fdatasync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    data = (char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  }

  ~ColdMapping() {
    munmap(data, size);
    ::close(fd);
  }
};

} // namespace

TEST(PagePrefetchTest, ProbeTurnsOnForColdPagesOnly) {
  std::vector<char> heap(8192, 'y');
  PagePrefetcher warm;
  warm.probe(heap.data());
  warm.refresh();
  EXPECT_FALSE(warm.active());

  ColdMapping cold;
  PagePrefetcher prefetch;
  prefetch.probe(cold.data + 10 * 4096);
  if (!prefetch.active()) GTEST_SKIP() << "page cache kept the test file resident";

  // Advised ranges coalesce and read in; reading them back is unaffected.
  prefetch.add(cold.data + 100, 300);
  prefetch.add(cold.data + 4000, 5000);
  prefetch.add(cold.data + 40 * 4096, 4096);
  prefetch.flush();
  long sum = 0;
  for (size_t i = 0; i < cold.size; i += 4096) sum += cold.data[i];
  EXPECT_EQ((long)(cold.size / 4096) * 'x', sum);
}
