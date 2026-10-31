// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <oneapi/tbb/task_group.h>

#include <filesystem>

#include "luxir/store/LazyInputFile.h"
#include "luxir/store/ObjectStore.h"

using namespace luxir;

namespace {

std::string pattern(size_t size) {
  std::string bytes(size, '\0');
  for (size_t i = 0; i < size; i++) bytes[i] = (char)('a' + i % 23);
  return bytes;
}

std::string_view view(InputStream& is) { return {is.ptr(), (size_t)is.left()}; }

class LazyInputFileTest : public ::testing::Test {
protected:
  std::shared_ptr<RamObjectStore> store = std::make_shared<RamObjectStore>();
  std::string bytes = pattern(100);

  std::shared_ptr<LazyInputFile> open(uint64_t chunk = 16) {
    store->put("obj", bytes);
    store->resetStats();
    auto file = std::make_shared<LazyInputFile>(store, "obj", bytes.size(), chunk);
    file->setPrefetchGap(0);  // strict: only adjacent ranges coalesce, unless a test says otherwise
    return file;
  }
};

}  // namespace

TEST_F(LazyInputFileTest, rangeFetchesOnlyTheCoveringChunks) {
  auto file = open();
  EXPECT_TRUE(file->lazy());
  EXPECT_EQ(0u, store->requests());  // opening a lazy file costs nothing

  auto is = file->rangeStream(20, 5);  // chunk 1 = [16, 32)
  EXPECT_EQ(bytes.substr(20, 5), view(is));
  EXPECT_EQ(1u, store->requests("read"));
  EXPECT_EQ(16u, store->bytesRead());

  is = file->rangeStream(14, 4);  // chunks 0 and 1; chunk 1 is resident
  EXPECT_EQ(bytes.substr(14, 4), view(is));
  EXPECT_EQ(2u, store->requests("read"));
  EXPECT_EQ(32u, file->fetchedBytes());

  file->rangeStream(18, 2);  // fully resident: no request
  EXPECT_EQ(2u, store->requests("read"));

  is = file->rangeStream(96, 4);  // the last chunk is short: [96, 100)
  EXPECT_EQ(bytes.substr(96, 4), view(is));
  EXPECT_EQ(36u, file->fetchedBytes());
  EXPECT_EQ(3u, file->residentChunkCount());
}

TEST_F(LazyInputFileTest, aRangeSpanningMissingRunsMakesOneRequestPerRun) {
  auto file = open();
  file->rangeStream(32, 1);  // chunk 2
  store->resetStats();
  file->rangeStream(0, 80);  // chunks 0-1 missing, 2 resident, 3-4 missing
  EXPECT_EQ(2u, store->requests("read"));
  auto is = file->rangeStream(0, 80);
  EXPECT_EQ(bytes.substr(0, 80), view(is));
}

TEST_F(LazyInputFileTest, prefetchCoalescesAdjacentRanges) {
  auto file = open();
  std::vector<ByteRange> ranges{{0, 10}, {10, 10}, {40, 5}, {90, 5}, {50, 0}};
  file->prefetchRanges(ranges);
  // [0,20) and [40,45) are chunks 0-1 and 2, adjacent: one request; 90 is chunk 5: another.
  EXPECT_EQ(2u, store->requests("read"));
  EXPECT_EQ(64u, file->fetchedBytes());  // chunks 0-2 and chunk 5
  file->rangeStream(0, 48);
  file->rangeStream(90, 5);
  EXPECT_EQ(2u, store->requests("read"));

  // Advisory: a range outside the file is clamped, not an error.
  std::vector<ByteRange> beyond{{98, 1000}, {5000, 1}};
  EXPECT_NO_THROW(file->prefetchRanges(beyond));
}

TEST_F(LazyInputFileTest, prefetchFetchesSmallGapsAlong) {
  std::vector<ByteRange> ranges{{0, 10}, {64, 5}};  // chunks 0 and 4, three chunks (48 bytes) apart
  auto file = open();
  file->setPrefetchGap(47);
  file->prefetchRanges(ranges);
  EXPECT_EQ(2u, store->requests("read"));

  file = open();
  file->setPrefetchGap(48);
  file->prefetchRanges(ranges);
  EXPECT_EQ(1u, store->requests("read"));
  EXPECT_EQ(80u, file->fetchedBytes());  // chunks 0-4, the gap included
  EXPECT_EQ(LazyInputFile::kDefaultPrefetchGap, 64u << 10);
}

TEST_F(LazyInputFileTest, readMaterializesTheWholeFileOnce) {
  auto file = open();
  file->rangeStream(0, 10);
  EXPECT_EQ(bytes, file->read());
  uint64_t requests = store->requests("read");
  EXPECT_EQ(bytes.size(), file->fetchedBytes());
  EXPECT_EQ(bytes, std::string(file->read()));
  auto is = file->getInputStream();
  EXPECT_EQ(bytes, view(is));
  EXPECT_EQ(requests, store->requests("read"));
}

TEST_F(LazyInputFileTest, concurrentReadersFetchEachChunkOnce) {
  bytes = pattern(1000);
  auto file = open(16);
  tbb::task_group group;
  for (int t = 0; t < 8; t++) {
    group.run([&, t] {
      for (int i = 0; i < 50; i++) {
        uint64_t offset = (uint64_t)((t * 37 + i * 91) % 900);
        auto is = file->rangeStream(offset, 100);
        EXPECT_EQ(bytes.substr(offset, 100), view(is));
      }
      EXPECT_EQ(bytes, file->read());
    });
  }
  group.wait();
  EXPECT_EQ(bytes.size(), store->bytesRead());  // never a chunk twice
  EXPECT_EQ(bytes.size(), file->fetchedBytes());
}

TEST_F(LazyInputFileTest, emptyAndOutOfRange) {
  store->put("empty", "");
  LazyInputFile empty(store, "empty", 0, 16);
  EXPECT_EQ(0u, empty.size());
  EXPECT_EQ(0u, empty.read().size());
  EXPECT_EQ(0, empty.rangeStream(0, 0).size());
  EXPECT_THROW(empty.rangeStream(0, 1), FileIOException);

  auto file = open();
  EXPECT_THROW(file->rangeStream(101, 0), FileIOException);
  EXPECT_THROW(file->rangeStream(90, 11), FileIOException);
  EXPECT_THROW(file->rangeStream(UINT64_MAX, 2), FileIOException);
  EXPECT_NO_THROW(file->rangeStream(100, 0));
}

TEST_F(LazyInputFileTest, aFailedFetchLeavesTheChunkMissing) {
  auto file = open();
  store->remove("obj");
  // A missing object reads as a missing file.
  EXPECT_THROW(file->rangeStream(0, 10), std::filesystem::filesystem_error);
  EXPECT_EQ(0u, file->residentChunkCount());
  // An object that does not match the read is no missing file.
  store->put("obj", bytes.substr(0, 5));
  try {
    file->rangeStream(0, 10);
    ADD_FAILURE() << "a short read must throw";
  } catch (const std::filesystem::filesystem_error&) {
    ADD_FAILURE() << "a short read is no missing file";
  } catch (const std::runtime_error&) {}
  EXPECT_EQ(0u, file->residentChunkCount());
  store->put("obj", bytes);
  auto is = file->rangeStream(0, 10);
  EXPECT_EQ(bytes.substr(0, 10), view(is));

  // A failed prefetch is advisory and does not throw.
  store->remove("obj");
  std::vector<ByteRange> ranges{{64, 10}};
  EXPECT_NO_THROW(file->prefetchRanges(ranges));
}

TEST_F(LazyInputFileTest, releaseCallbackRunsOnDestruction) {
  store->put("obj", bytes);
  int released = 0;
  {
    LazyInputFile file(store, "obj", bytes.size(), 16, [&] { released++; });
    EXPECT_EQ(0, released);
  }
  EXPECT_EQ(1, released);
}

TEST_F(LazyInputFileTest, defaultRangeStreamSlicesTheWholeStream) {
  RAMInputFile ram(std::make_unique_for_overwrite<char[]>(10), 10);
  EXPECT_FALSE(ram.lazy());
  EXPECT_EQ(4, ram.rangeStream(3, 4).size());
  EXPECT_EQ(0, ram.rangeStream(10, 0).left());
  EXPECT_THROW(ram.rangeStream(8, 3), FileIOException);
  std::vector<ByteRange> ranges{{0, 5}};
  EXPECT_NO_THROW(ram.prefetchRanges(ranges));  // memory needs no readahead
}
