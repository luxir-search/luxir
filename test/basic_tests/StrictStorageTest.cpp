// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <thread>

#include "luxir/store/AcquireJoin.h"
#include "luxir/store/Manifest.h"
#include "luxir/store/VerifiedDirectory.h"
#include "test/GatedStorage.h"
#include "test/LuxirTest.h"
#include "test/StrictStorage.h"

#if defined(LUXIR_ASAN)
#include <sanitizer/asan_interface.h>
#endif

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr std::string_view payload = "0123456789abcdefghijklmnopqrstuvwxyz";

FileDescriptor put(Directory& dir, std::string_view name) {
  auto file = dir.createFile(name);
  OutputStream out(file.get());
  out.writeBytes(payload);
  out.close();
  dir.finishFile(*file);
  return file->descriptor();
}

std::string_view contents(const HeldRange& hold) {
  return {hold.bytes().data(), hold.bytes().size()};
}

// Completes each batch on its own thread, as a remote backend would.
class ThreadedInputFile : public InputFile {
  std::shared_ptr<InputFile> delegate;
  std::vector<std::jthread> threads;

public:
  explicit ThreadedInputFile(std::shared_ptr<InputFile> delegate) : delegate(std::move(delegate)) {}
  ~ThreadedInputFile() override { threads.clear(); }
  size_t size() override { return delegate->size(); }
  HeldRange acquire(ByteRange range) override { return delegate->acquire(range); }
  void acquireBatch(std::span<const ByteRange> ranges, AcquireDone done) noexcept override {
    try {
      threads.emplace_back([this, requested = std::vector(ranges.begin(), ranges.end()), done] {
        delegate->acquireBatch(requested, done);
      });
    } catch (...) {
      done({}, std::current_exception());
    }
  }
};

} // namespace

TEST(StrictStorageTest, copiesWithVaryingGarbageAndFileAlignment) {
  auto ram = std::make_shared<RAMDir>();
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(ram, counters);
  put(dir, "data");
  auto file = dir.openFile("data");
  ASSERT_NE(nullptr, file);
  EXPECT_FALSE(file->holdsFree());
  auto first = file->acquire({3, 5});
  auto second = file->acquire({3, 5});
  EXPECT_EQ("34567", contents(first));
  EXPECT_EQ(contents(first), contents(second));
  auto original = ram->openFile("data");
  auto originalPart = original->acquire({3, 5});
  EXPECT_NE(originalPart.getData(), first.getData());
  EXPECT_NE(first.getData(), second.getData());
  EXPECT_EQ(3u, (uintptr_t)first.getData() % kHoldAlign);
  EXPECT_NE(first.getData()[5 + kReadSlack - 1], second.getData()[5 + kReadSlack - 1]);
  EXPECT_THROW(file->acquire({1, UINT64_MAX}), std::out_of_range);
  auto whole = file->acquire({0, file->size()});
  EXPECT_EQ(payload, contents(whole));

  auto stats = counters->fileStats("data");
  EXPECT_EQ(3u, stats.acquires);
  EXPECT_EQ(10 + payload.size(), stats.acquiredBytes);
  EXPECT_EQ(1u, stats.wholeFileAccesses);
  ASSERT_EQ(2u, stats.parts.size());
  EXPECT_TRUE(stats.parts.contains({3, 5}));
  EXPECT_TRUE(stats.parts.contains({0, payload.size()}));
  EXPECT_EQ(0u, counters->fileStats("missing").acquires);
  EXPECT_EQ(3u, counters->liveStats().peakHolds);
  EXPECT_EQ(10 + payload.size(), counters->liveStats().peakBytes);
}

TEST(StrictStorageTest, slackStopsAtEof) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::make_shared<RAMDir>(), counters);
  put(dir, "data");
  auto file = dir.openFile("data");
  ASSERT_NE(nullptr, file);
  for (size_t slack = 0; slack <= kReadSlack; ++slack) {
    size_t offset = file->size() - slack - 1;
    auto hold = file->acquire({offset, 1});
    const char* data = hold.getData();
    EXPECT_EQ(payload[offset], data[0]);
    EXPECT_EQ(offset % kHoldAlign, (uintptr_t)data % kHoldAlign);
#if defined(LUXIR_ASAN)
    EXPECT_TRUE(__asan_address_is_poisoned(data - 8));
    EXPECT_FALSE(__asan_address_is_poisoned(data + slack));
    EXPECT_TRUE(__asan_address_is_poisoned(data + slack + 1));
#endif
  }
  auto empty = file->acquire({file->size(), 0});
  EXPECT_TRUE(empty.bytes().empty());
  EXPECT_EQ(0, empty.stream().left());
}

TEST(StrictStorageTest, movesReleaseOnceAcrossThreads) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::make_shared<RAMDir>(), counters);
  put(dir, "data");
  auto file = dir.openFile("data");
  ASSERT_NE(nullptr, file);
  auto kept = file->acquire({0, 2});
  {
    auto incoming = file->acquire({2, 3});
    auto moved = std::move(incoming);
    kept = std::move(moved);
    EXPECT_EQ("234", contents(kept));
    EXPECT_EQ(1u, counters->liveStats().holds);
    EXPECT_EQ(3u, counters->liveStats().bytes);
  }
  EXPECT_EQ(1u, counters->liveStats().holds);
  {
    std::jthread thread([incoming = std::move(kept)]() mutable {
      auto hold = std::move(incoming);
      EXPECT_EQ("234", contents(hold));
    });
  }
  EXPECT_EQ(0u, counters->liveStats().holds);
  EXPECT_EQ(0u, counters->liveStats().bytes);
  EXPECT_EQ(2u, counters->liveStats().peakHolds);
  EXPECT_EQ(5u, counters->liveStats().peakBytes);
}

TEST(StrictStorageTest, deniesWholeBaseSegmentFiles) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::make_shared<RAMDir>(), counters, StrictWholeFiles::DENY);
  auto key = Postings::segmentKey(1, 1);
  auto name = Postings::getIndexFileName(key, 0u);
  put(dir, name);
  auto file = dir.openFile(name);
  ASSERT_NE(nullptr, file);
  EXPECT_THROW(file->acquire({0, file->size()}), FileIOException);
  auto part = file->acquire({0, 2});
  EXPECT_EQ("01", contents(part));
  EXPECT_EQ(1u, counters->fileStats(name).wholeFileAccesses);
  EXPECT_EQ(1u, counters->fileStats(name).acquires);

  for (const auto& allowed : {Manifest::name(1, 1), Postings::getLiveDocsFileName(key, 1, 1),
                             Postings::getSegmentOverlayFileName(1, "vec.title", 1, 0, 1, 1), name + ".tmp"}) {
    put(dir, allowed);
    auto other = dir.openFile(allowed);
    ASSERT_NE(nullptr, other);
    auto whole = other->acquire({0, other->size()});
    EXPECT_EQ(payload, contents(whole));
  }
}

TEST(StrictStorageTest, rejectsAPartAcquiredUnderBothClasses) {
  StrictDirectory dir(std::make_shared<RAMDir>(), std::make_shared<StrictStorageCounters>());
  put(dir, "data");
  auto file = dir.openFile("data");
  EXPECT_NO_THROW(file->acquire({4, 8, ReadClass::METADATA}));
  EXPECT_NO_THROW(file->acquire({4, 8, ReadClass::METADATA}));
  EXPECT_NO_THROW(file->acquire({4, 9}));
  EXPECT_THROW(file->acquire({4, 8}), std::logic_error);
}

// Reads no part makes count as whole-file reads: from offset 0 past the
// header, to EOF outside file 0, and any that leaves out at most a header's
// worth of bytes, which includes file 0 past its header.
TEST(StrictStorageTest, deniesReadsNoSegmentPartMakes) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::make_shared<RAMDir>(), counters, StrictWholeFiles::DENY);
  auto header = Postings::LUXIR_HEADER.size();
  for (uint32_t number : {0u, 1u}) {
    auto name = Postings::getIndexFileName(Postings::segmentKey(1, 1), number);
    put(dir, name);
    auto file = dir.openFile(name);
    auto size = file->size();
    EXPECT_NO_THROW(file->acquire({0, header}));
    EXPECT_THROW(file->acquire({0, header + 1}), FileIOException);
    EXPECT_THROW(file->acquire({header, size - header}), FileIOException);
    EXPECT_THROW(file->acquire({1, size - header}), FileIOException);
    EXPECT_NO_THROW(file->acquire({header, size - header - 1}));
    if (number == 0) EXPECT_NO_THROW(file->acquire({header + 1, size - header - 1}));
    else EXPECT_THROW(file->acquire({header + 1, size - header - 1}), FileIOException);
  }
}

// Other files count reads leaving out at most a header's worth of bytes as
// whole, and deny does not apply to them.
TEST(StrictStorageTest, countsNearWholeReadsOfAnyFile) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::make_shared<RAMDir>(), counters, StrictWholeFiles::DENY);
  auto header = Postings::LUXIR_HEADER.size();
  auto name = Postings::getLiveDocsFileName(Postings::segmentKey(1, 1), 1, 1);
  put(dir, name);
  auto file = dir.openFile(name);
  auto size = file->size();
  ByteRange whole[] = {{0, size}, {header, size - header}, {0, size - header}, {1, size - 2}};
  for (auto range : whole) EXPECT_NO_THROW(file->acquire(range));
  EXPECT_NO_THROW(file->acquire({header, size - header - 1}));
  EXPECT_NO_THROW(file->acquire({0, size - header - 1}));
  EXPECT_EQ(4u, counters->fileStats(name).wholeFileAccesses);
}

TEST(StrictStorageTest, copyHoldsBoundedChunksOfItsSource) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory source(std::make_shared<RAMDir>(), counters);
  RAMDir dest;
  std::string bytes(2 * Directory::kCopyChunk + 100, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = (char)(i * 31 % 251);
  {
    auto file = source.createFile("data");
    OutputStream out(file.get());
    out.write(bytes.data(), bytes.size());
    out.close();
    source.finishFile(*file);
  }
  dest.copyFile(source, "data");
  auto copied = dest.openFile("data");
  ASSERT_NE(nullptr, copied);
  auto hold = copied->acquire({0, copied->size()});
  EXPECT_EQ(bytes, contents(hold));
  auto stats = counters->fileStats("data");
  EXPECT_EQ(3u, stats.acquires);
  EXPECT_EQ(0u, stats.wholeFileAccesses);
  EXPECT_EQ(Directory::kCopyChunk, counters->liveStats().peakBytes);
  EXPECT_EQ(1u, counters->liveStats().peakHolds);
}

TEST(StrictStorageTest, wrapsVerifiedMappingAfterDirectoryCloses) {
  auto counters = std::make_shared<StrictStorageCounters>();
  std::shared_ptr<InputFile> file;
  {
    StrictDirectory dir(std::make_shared<RAMDir>(), counters);
    auto desc = put(dir, "data");
    FileVerification verification;
    verification.open(dir, std::span(&desc, 1), true, {});
    verification.run();
    VerifiedDirectory verified(dir, verification);
    ASSERT_TRUE(dir.deleteFile(desc.name));
    file = verified.openFile(desc.name);
  }
  ASSERT_NE(nullptr, file);
  EXPECT_FALSE(file->holdsFree());
  auto hold = file->acquire({0, 2});
  EXPECT_EQ("01", contents(hold));
  EXPECT_EQ(1u, counters->fileStats("data").acquires);
}

TEST(StrictStorageTest, reuseCopiesWrappedRamFiles) {
  auto counters = std::make_shared<StrictStorageCounters>();
  auto ram = std::make_shared<RAMDir>();
  StrictDirectory source(std::make_shared<RAMDir>(), counters), dest(ram, counters);
  auto files = std::array{put(source, "data")};
  EXPECT_EQ(ram->capabilities() & ~Directory::HARD_LINK, dest.capabilities());
  ASSERT_EQ(std::vector<FileDescriptor>(files.begin(), files.end()), dest.reuseFiles(source, files, files));
  auto copied = ram->openFile("data");
  ASSERT_NE(nullptr, copied);
  auto hold = copied->acquire({0, copied->size()});
  EXPECT_EQ(payload, contents(hold));
  EXPECT_EQ(1u, counters->fileStats("data").wholeFileAccesses);
}

TEST(StrictStorageTest, ramLinkSharesFileFromWrappedSource) {
  auto ram = std::make_shared<RAMDir>();
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory source(ram, counters);
  put(source, "data");
  RAMDir dest;
  dest.linkFile(source, "data");
  auto linked = dest.openFile("data");
  ASSERT_NE(nullptr, linked);
  EXPECT_EQ(ram->openFile("data"), linked);
  EXPECT_EQ(0u, counters->fileStats("data").acquires);
}

TEST(StrictStorageTest, batchesForwardAsOneSubmission) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::make_shared<RAMDir>(), counters);
  put(dir, "data");
  auto file = dir.openFile("data");
  ASSERT_NE(nullptr, file);
  ByteRange ranges[] = {{3, 5}, {20, 4, ReadClass::METADATA}};
  auto holds = file->acquireAll(ranges);
  ASSERT_EQ(2u, holds.size());
  EXPECT_EQ("34567", contents(holds[0]));
  EXPECT_EQ("klmn", contents(holds[1]));
  EXPECT_EQ(20u % kHoldAlign, (uintptr_t)holds[1].getData() % kHoldAlign);
  auto single = file->acquire({0, 2});
  ByteRange outside[] = {{0, 2}, {1, UINT64_MAX}};
  EXPECT_THROW(file->acquireAll(outside), std::out_of_range);

  auto stats = counters->fileStats("data");
  EXPECT_EQ(3u, stats.acquires);
  EXPECT_EQ(1u, stats.metadataAcquires);
  EXPECT_EQ(1u, stats.batches);  // the rejected batch never went out
  EXPECT_EQ(2u, stats.submissions);
  EXPECT_EQ(1u, stats.firstSubmission);
  EXPECT_EQ(2u, stats.lastSubmission);
  EXPECT_EQ(2u, counters->totalSubmissions());
}

TEST(StrictStorageTest, batchReportsErrorsThroughDone) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::make_shared<RAMDir>(), counters, StrictWholeFiles::DENY);
  auto name = Postings::getIndexFileName(Postings::segmentKey(1, 1), 0u);
  put(dir, name);
  auto file = dir.openFile(name);
  ASSERT_NE(nullptr, file);
  ByteRange outside[] = {{0, 2}, {1, UINT64_MAX}};
  ByteRange whole[] = {{0, 2}, {0, file->size()}};
  for (auto ranges : {std::span<const ByteRange>(outside), std::span<const ByteRange>(whole)}) {
    int calls = 0;
    std::exception_ptr error;
    file->acquireBatch(ranges, [&](std::vector<HeldRange>&& holds, std::exception_ptr failure) {
      ++calls;
      EXPECT_TRUE(holds.empty());
      error = failure;
    });
    EXPECT_EQ(1, calls);
    EXPECT_NE(nullptr, error);
  }
  EXPECT_EQ(0u, counters->fileStats(name).batches);
}

TEST(StrictStorageTest, joinWaitsForBatchesCompletingElsewhere) {
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::make_shared<RAMDir>(), counters);
  put(dir, "a");
  put(dir, "b");
  ThreadedInputFile a(dir.openFile("a")), b(dir.openFile("b"));
  std::vector<HeldRange> fromA, fromB;
  ByteRange ranges[] = {{0, 2}, {10, 1}};
  {
    AcquireJoin join;
    join.acquire(a, ranges, [&](auto&& holds) { fromA = std::move(holds); });
    join.acquire(b, std::span(ranges, 1), [&](auto&& holds) { fromB = std::move(holds); });
    join.wait();
  }
  ASSERT_EQ(2u, fromA.size());
  ASSERT_EQ(1u, fromB.size());
  EXPECT_EQ("a", contents(fromA[1]));
  EXPECT_EQ("01", contents(fromB[0]));
  EXPECT_EQ(1u, counters->fileStats("a").batches);
  EXPECT_EQ(1u, counters->fileStats("b").batches);

  AcquireJoin failing;
  ByteRange outside[] = {{1, UINT64_MAX}};
  failing.acquire(a, outside, [](auto&&) { FAIL(); });
  failing.acquire(b, ranges, [](auto&&) { throw FileIOException("take failed"); });
  EXPECT_ANY_THROW(failing.wait());
  EXPECT_EQ(3u, counters->liveStats().holds);  // the failed take's holds released before wait() returned
  fromA.clear();
  fromB.clear();
}

TEST(StrictStorageTest, joinDestroyedWithoutWaitWaitsForItsBatches) {
  auto counters = std::make_shared<StrictStorageCounters>();
  auto strict = std::make_shared<StrictDirectory>(std::make_shared<RAMDir>(), counters);
  put(*strict, "a");
  GatedDirectory dir(strict, 2, std::chrono::milliseconds(50));  // Holds the batch for one that never comes.
  auto file = dir.openFile("a");
  std::vector<HeldRange> taken;
  ByteRange ranges[] = {{0, 2}};
  {
    AcquireJoin join;
    join.acquire(*file, ranges, [&](auto&& holds) { taken = std::move(holds); });
  }
  EXPECT_EQ(1u, taken.size());
  EXPECT_TRUE(dir.gate.timedOut());
}
