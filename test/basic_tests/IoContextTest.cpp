// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "luxir/store/LazyInputFile.h"
#include "luxir/util/IoContext.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/thread.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;

namespace {

// A RAM object store that records the context each range read sees.
class RecordingStore : public RamObjectStore {
public:
  std::mutex mutex;
  std::vector<std::optional<IoContext>> seen;

  void readRange(std::string_view name, uint64_t offset, std::span<char> out) override {
    {
      std::lock_guard lock(mutex);
      seen.push_back(captureIoContext());
    }
    RamObjectStore::readRange(name, offset, out);
  }
};

IoContext context(uint64_t queryId, IoCounters* counters = nullptr) {
  IoContext io;
  io.queryId = queryId;
  io.counters = counters;
  return io;
}

constexpr uint64_t kChunk = 1024;
constexpr size_t kChunks = 8;

struct LazyFixture {
  std::shared_ptr<RecordingStore> store = std::make_shared<RecordingStore>();
  std::unique_ptr<LazyInputFile> file;
  LazyFixture() {
    store->put("f", std::string(kChunk * kChunks, 'x'));
    file = std::make_unique<LazyInputFile>(store, "f", kChunk * kChunks, kChunk);
  }
  // Touches 100 bytes of each chunk, one task per chunk.
  void readEachChunkInTasks() {
    TaskGroupRunner runner;
    task_group_fanout(runner.group(), kChunks, [&](size_t i) {
      return [this, i] { file->rangeStream(i * kChunk, 100); };
    });
    runner.join();
  }
};

}  // namespace

TEST(IoContextTest, tasksSeeTheSpawnersContextInObjectStoreReads) {
  LazyFixture f;
  IoCounters counters;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
  IoContext io = context(7, &counters);
  io.requestId = "r7";
  io.deadline = deadline;
  io.fillCache = false;
  {
    IoScope scope(&io);
    f.readEachChunkInTasks();
  }
  EXPECT_EQ(nullptr, IoContext::current());
  ASSERT_EQ(kChunks, f.store->seen.size());
  for (const auto& seen : f.store->seen) {
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(7u, seen->queryId);
    EXPECT_EQ("r7", seen->requestId);
    EXPECT_EQ(deadline, seen->deadline);
    EXPECT_FALSE(seen->fillCache);
    EXPECT_EQ(&counters, seen->counters);
  }
  auto totals = counters.snapshot();
  EXPECT_EQ(kChunks, totals.requests);
  EXPECT_EQ(kChunks, totals.rounds);
  EXPECT_EQ(kChunks * kChunk, totals.fetchedBytes);
  EXPECT_EQ(kChunks * 100, totals.touchedBytes);
  EXPECT_EQ(0u, totals.wholeFileReads);

  // Resident now: touching again costs no requests. A whole-file read counts.
  {
    IoScope scope(&io);
    f.file->read();
  }
  totals = counters.snapshot();
  EXPECT_EQ(kChunks, totals.requests);
  EXPECT_EQ(kChunks * 100 + kChunks * kChunk, totals.touchedBytes);
  EXPECT_EQ(1u, totals.wholeFileReads);
}

TEST(IoContextTest, readsOutsideAnyRequestSeeNoContext) {
  LazyFixture f;
  IoCounters counters;
  IoContext io = context(8, &counters);
  {
    IoScope scope(&io);
    f.file->rangeStream(0, 1);
  }
  // The same worker threads, now without a context: nothing leaked to them.
  f.store->seen.clear();
  f.readEachChunkInTasks();
  ASSERT_EQ(kChunks - 1, f.store->seen.size());  // chunk 0 is resident
  for (const auto& seen : f.store->seen) EXPECT_FALSE(seen.has_value());
  EXPECT_EQ(1u, counters.snapshot().requests);
}

TEST(IoContextTest, boundWorkRestoresTheContextOfTheThreadThatRunsIt) {
  IoContext request = context(1);
  IoContext other = context(2);
  auto queryId = [] { return IoContext::current() ? IoContext::current()->queryId : 0; };
  std::vector<uint64_t> inside;
  auto task = [&] {
    IoScope scope(&request);
    return bindIoContext([&] { inside.push_back(queryId()); });
  }();
  auto failing = [&] {
    IoScope scope(&request);
    return bindIoContext([] { throw std::runtime_error("boom"); });
  }();
  uint64_t before = 9, after = 9, afterNested = 9, afterThrow = 9;
  std::thread([&] {
    before = queryId();
    task();
    after = queryId();
    IoScope scope(&other);
    task();
    afterNested = queryId();
    try { failing(); } catch (const std::exception&) {}
    afterThrow = queryId();
  }).join();
  EXPECT_EQ(0u, before);
  EXPECT_EQ((std::vector<uint64_t>{1, 1}), inside);
  EXPECT_EQ(0u, after);
  EXPECT_EQ(2u, afterNested);
  EXPECT_EQ(2u, afterThrow);

  // Shared work keeps the request's counters but none of its policy.
  IoCounters counters;
  IoContext io = context(3, &counters);
  io.deadline = std::chrono::steady_clock::now();
  io.fillCache = false;
  auto shared = io.shared();
  EXPECT_EQ(3u, shared.queryId);
  EXPECT_EQ(&counters, shared.counters);
  EXPECT_FALSE(shared.hasDeadline());
  EXPECT_TRUE(shared.fillCache);
}

// A search over a collection on the ram_object backend reports the requests
// and bytes it fetched, exactly as the store counted them.
TEST(IoContextTest, searchProfileCountsObjectStoreIo) {
  using namespace luxir::test;
  std::string name = "io_" + newUuid();
  LuxirConfig config;
  config.store.backend = "ram_object";
  config.store.options = {{"ram_object", {{"store_name", name}, {"chunk_bytes", "1K"}}}};
  auto store = RamObjectStore::named(name);
  std::string filler(200, 'a');
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    for (int seg = 0; seg < 3; seg++) {
      std::vector<Doc> docs;
      for (int i = 0; i < 100; i++) {
        std::string id = std::to_string(seg) + "_" + std::to_string(i);
        docs.push_back(flatdoc("id", id, "text_un", (i == 50 ? "needle " : "hay ") + filler + id));
      }
      ASSERT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }
  {  // reopened: the segments are read lazily from the store
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    auto search = [&] {
      auto req = localReq(h.getSearchEngine());
      req->collection("main").profile().topDocs("q").matchQuery("text_un", "needle").fields({"id", "text_un"});
      req->execute(true);
      EXPECT_OK(req);
      return req->responses.back()->proto.profile.value_or(api::ExecutionProfile{});
    };
    auto requests = store->requests("read");
    auto bytes = store->bytesRead();
    auto metricRequests = node.metrics->counts().counters[(size_t)MetricCounter::SEARCH_IO_REQUESTS];
    auto cold = search();
    EXPECT_GT(cold.io_requests, 0u);
    EXPECT_EQ(store->requests("read") - requests, cold.io_requests);
    EXPECT_EQ(store->bytesRead() - bytes, cold.io_fetched_bytes);
    EXPECT_EQ(cold.io_requests, cold.io_rounds);
    EXPECT_GE(cold.io_touched_bytes, 3 * filler.size());  // three returned documents at least
    EXPECT_EQ(metricRequests + cold.io_requests,
              node.metrics->counts().counters[(size_t)MetricCounter::SEARCH_IO_REQUESTS]);

    auto warm = search();  // everything it needs is resident now
    EXPECT_EQ(0u, warm.io_requests);
    EXPECT_EQ(0u, warm.io_fetched_bytes);
    EXPECT_LE(warm.io_touched_bytes, cold.io_touched_bytes);
  }
  store.reset();
  RamObjectStore::drop(name);
}
