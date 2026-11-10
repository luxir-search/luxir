// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <oneapi/tbb/task_arena.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "luxir/store/FetchCountdown.h"

using namespace luxir;
namespace tbb = oneapi::tbb;

namespace {

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

// Holds fetches until the test opens it.
class Gate {
  std::mutex mutex;
  std::condition_variable opened;
  bool isOpen = false;

public:
  void wait() {
    std::unique_lock lock(mutex);
    opened.wait(lock, [&] { return isOpen; });
  }
  void open() {
    {
      std::lock_guard lock(mutex);
      isOpen = true;
    }
    opened.notify_all();
  }
};

// A lazy file over a string whose fetches complete on a thread of their own,
// once the gate opens.
class GatedFile : public InputFile {
  std::string bytes;
  Gate& gate;
  std::vector<std::thread> threads;

public:
  std::atomic<int> completed{0};

  GatedFile(std::string bytes, Gate& gate) : bytes(std::move(bytes)), gate(gate) {}
  ~GatedFile() override {
    for (auto& thread : threads) thread.join();
  }

  size_t size() override { return bytes.size(); }
  std::string_view read() override { return bytes; }
  InputStream getInputStream() override { return InputStream(bytes.data(), bytes.data() + bytes.size()); }
  bool lazy() const override { return true; }

  void fetchRanges(std::span<const ByteRange> ranges, ReadDeadline, FetchDone done) override {
    std::exception_ptr error;
    try {
      for (const auto& range : ranges) checkRange(range.offset, range.length, bytes.size());
    } catch (...) {
      error = std::current_exception();
    }
    threads.emplace_back([this, error, done = std::move(done)] {
      gate.wait();
      completed++;
      done(error);
    });
  }
};

}  // namespace

TEST(RangeFetchTest, filesThatNeedNoFetchCompleteInline) {
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

TEST(RangeFetchTest, countdownLetsTheCallerGoOnWhenNothingWaits) {
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

TEST(RangeFetchTest, countdownResumesOnTheArenaAfterTheLastFetch) {
  tbb::task_arena arena(2);
  Gate gate;
  std::weak_ptr<GatedFile> watched;
  std::promise<std::pair<std::exception_ptr, bool>> resumed;
  {
    auto a = std::make_shared<GatedFile>(std::string(100, 'a'), gate);
    auto b = std::make_shared<GatedFile>(std::string(100, 'b'), gate);
    watched = a;
    std::weak_ptr<GatedFile> wa = a, wb = b;
    auto countdown = FetchCountdown::create(arena, [&, wa, wb](std::exception_ptr error) {
      auto a = wa.lock(), b = wb.lock();  // still held by the countdown
      bool allDone = a && b && a->completed == 1 && b->completed == 2;
      resumed.set_value({error, allDone && tbb::this_task_arena::current_thread_index() >= 0});
    });
    countdown->fetch(a, std::vector<ByteRange>{{0, 20}}, kNoReadDeadline);
    countdown->fetch(b, std::vector<ByteRange>{{50, 5}}, kNoReadDeadline);
    countdown->fetch(b, std::vector<ByteRange>{{60, 50}}, kNoReadDeadline);  // outside b: the first error
    EXPECT_FALSE(countdown->arm());
  }
  EXPECT_FALSE(watched.expired());  // the countdown holds its files
  gate.open();
  auto [error, onArena] = resumed.get_future().get();
  EXPECT_TRUE(failsWith<FileIOException>(error));
  EXPECT_TRUE(onArena);
}
