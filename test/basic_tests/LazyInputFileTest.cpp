// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <oneapi/tbb/task_group.h>

#include <condition_variable>
#include <filesystem>
#include <future>

#include "luxir/store/FetchCountdown.h"
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

// A store whose reads wait until the test opens the gate.
class GatedStore : public RamObjectStore {
  std::mutex mutex;
  std::condition_variable opened;
  bool open = false;

public:
  void readRange(std::string_view name, uint64_t offset, std::span<char> out) override {
    {
      std::unique_lock lock(mutex);
      opened.wait(lock, [&] { return open; });
    }
    RamObjectStore::readRange(name, offset, out);
  }
  void openGate() {
    {
      std::lock_guard lock(mutex);
      open = true;
    }
    opened.notify_all();
  }
};

// Starts fetchRanges; the future completes with what done received.
std::future<std::exception_ptr> fetch(InputFile& file, std::vector<ByteRange> ranges,
                                      ReadDeadline deadline = kNoReadDeadline) {
  auto promise = std::make_shared<std::promise<std::exception_ptr>>();
  auto result = promise->get_future();
  file.fetchRanges(ranges, deadline, [promise](std::exception_ptr error) { promise->set_value(error); });
  return result;
}

template <typename E>
bool failsWith(std::exception_ptr error) {
  try {
    if (error) std::rethrow_exception(error);
  } catch (const E&) {
    return true;
  } catch (...) {}
  return false;
}

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
  file->drain();
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
  file->drain();
  EXPECT_EQ(2u, store->requests("read"));

  file = open();
  file->setPrefetchGap(48);
  file->prefetchRanges(ranges);
  file->drain();
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
  file->drain();
  EXPECT_NO_THROW(file->prefetchRangesNow(ranges));
  EXPECT_EQ(1u, file->residentChunkCount());
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

TEST_F(LazyInputFileTest, fetchRangesMakesTheRangesResident) {
  auto file = open();
  auto done = fetch(*file, {{0, 10}, {10, 10}, {40, 5}, {90, 5}});
  EXPECT_EQ(nullptr, done.get());
  EXPECT_EQ(2u, store->requests("read"));  // chunks 0-2 coalesce; chunk 5 is another
  auto is = file->rangeStream(40, 5);
  EXPECT_EQ(bytes.substr(40, 5), view(is));
  EXPECT_EQ(2u, store->requests("read"));

  // Resident ranges complete inline, before fetchRanges returns.
  auto again = fetch(*file, {{2, 3}});
  EXPECT_EQ(std::future_status::ready, again.wait_for(std::chrono::seconds(0)));
  EXPECT_EQ(nullptr, again.get());
}

TEST_F(LazyInputFileTest, fetchRangesDoesNotHoldTheCaller) {
  auto gated = std::make_shared<GatedStore>();
  gated->put("obj", bytes);
  auto file = std::make_shared<LazyInputFile>(gated, "obj", bytes.size(), 16);
  auto done = fetch(*file, {{20, 30}});
  // The store has not answered, yet fetchRanges returned.
  EXPECT_EQ(std::future_status::timeout, done.wait_for(std::chrono::milliseconds(20)));
  gated->openGate();
  EXPECT_EQ(nullptr, done.get());
  EXPECT_EQ(3u, file->residentChunkCount());

  // prefetchRanges does not wait either.
  file->prefetchRanges(std::vector<ByteRange>{{80, 10}});
  file->drain();
  EXPECT_EQ(4u, file->residentChunkCount());
}

TEST_F(LazyInputFileTest, fetchRangesReportsFailures) {
  auto file = open();
  // A range outside the file fails inline and fetches nothing.
  auto outside = fetch(*file, {{0, 10}, {95, 10}});
  EXPECT_EQ(std::future_status::ready, outside.wait_for(std::chrono::seconds(0)));
  EXPECT_TRUE(failsWith<FileIOException>(outside.get()));
  EXPECT_EQ(0u, store->requests("read"));

  // A deadline that passed fails without a request.
  auto late = fetch(*file, {{0, 10}}, std::chrono::steady_clock::now() - std::chrono::seconds(1));
  EXPECT_TRUE(failsWith<FetchTimeoutError>(late.get()));
  EXPECT_EQ(0u, store->requests("read"));

  // A missing object reports a missing file and leaves the chunks missing.
  store->remove("obj");
  EXPECT_TRUE(failsWith<std::filesystem::filesystem_error>(fetch(*file, {{0, 10}}).get()));
  EXPECT_EQ(0u, file->residentChunkCount());
}

TEST_F(LazyInputFileTest, filesThatNeedNoFetchCompleteInline) {
  RAMInputFile ram(std::make_unique_for_overwrite<char[]>(10), 10);
  auto done = fetch(ram, {{0, 10}});
  EXPECT_EQ(std::future_status::ready, done.wait_for(std::chrono::seconds(0)));
  EXPECT_EQ(nullptr, done.get());
  EXPECT_TRUE(failsWith<FileIOException>(fetch(ram, {{8, 3}}).get()));

  // A file that cannot fetch ahead says so, and its reads still work.
  struct Plain : InputFile {
    std::string bytes = "abc";
    size_t size() override { return bytes.size(); }
    std::string_view read() override { return bytes; }
    InputStream getInputStream() override { return InputStream(bytes.data(), bytes.data() + bytes.size()); }
  } plain;
  auto error = fetch(plain, {{0, 3}}).get();
  EXPECT_TRUE(isFetchNotResident(error));
  EXPECT_FALSE(isFetchNotResident(nullptr));
}

TEST_F(LazyInputFileTest, countdownLetsTheCallerGoOnWhenNothingWaits) {
  tbb::task_arena arena(2);
  auto ram = std::make_shared<RAMInputFile>(std::make_unique_for_overwrite<char[]>(10), 10);
  int runs = 0;
  auto countdown = FetchCountdown::create(arena, [&](std::exception_ptr) { runs++; });
  countdown->fetch(ram, std::vector<ByteRange>{{0, 4}}, kNoReadDeadline);
  countdown->fetch(ReadSet{{ram, {{4, 6}}}, {ram, {{9, 2}}}}, kNoReadDeadline);
  EXPECT_TRUE(countdown->arm());
  EXPECT_EQ(0, runs);  // the caller goes on: no continuation
  EXPECT_TRUE(failsWith<FileIOException>(countdown->failure()));  // [9, +2) leaves the file

  auto empty = FetchCountdown::create(arena, [&](std::exception_ptr) { runs++; });
  EXPECT_TRUE(empty->arm());
  EXPECT_EQ(nullptr, empty->failure());
  EXPECT_EQ(0, runs);
}

TEST_F(LazyInputFileTest, countdownResumesOnTheArenaAfterTheLastFetch) {
  tbb::task_arena arena(2);
  auto gated = std::make_shared<GatedStore>();
  gated->put("a", bytes);
  gated->put("b", bytes);
  std::weak_ptr<LazyInputFile> watched;
  std::promise<std::pair<std::exception_ptr, bool>> resumed;
  {
    auto a = std::make_shared<LazyInputFile>(gated, "a", bytes.size(), 16);
    auto b = std::make_shared<LazyInputFile>(gated, "b", bytes.size(), 16);
    watched = a;
    std::weak_ptr<LazyInputFile> wa = a, wb = b;
    auto countdown = FetchCountdown::create(arena, [&, wa, wb](std::exception_ptr error) {
      auto a = wa.lock(), b = wb.lock();  // still held by the countdown
      bool resident = a && b && a->residentChunkCount() == 2 && b->residentChunkCount() == 1;
      resumed.set_value({error, resident && tbb::this_task_arena::current_thread_index() >= 0});
    });
    countdown->fetch(a, std::vector<ByteRange>{{0, 20}}, kNoReadDeadline);
    countdown->fetch(b, std::vector<ByteRange>{{50, 5}}, kNoReadDeadline);
    countdown->fetch(b, std::vector<ByteRange>{{60, 50}}, kNoReadDeadline);  // outside b: the first error
    EXPECT_FALSE(countdown->arm());
  }
  EXPECT_FALSE(watched.expired());  // the countdown holds its files
  gated->openGate();
  auto [error, onArena] = resumed.get_future().get();
  EXPECT_TRUE(failsWith<FileIOException>(error));
  EXPECT_TRUE(onArena);
}
