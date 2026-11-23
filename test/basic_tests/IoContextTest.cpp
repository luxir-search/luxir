// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/util/IoContext.h"
#include "luxir/util/thread.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace luxir;

namespace {

// Acquires the io_held layer saw while a request context was installed.
struct AcquireTotals {
  std::atomic<uint64_t> ranges{0};
  std::atomic<uint64_t> bytes{0};
};
AcquireTotals acquireTotals;

// Hands out the inner file's views but reports them as held, as a backend
// that copies ranges does, so segments acquire and retain each part.
class HeldInputFile : public InputFile {
  std::shared_ptr<InputFile> inner;

public:
  explicit HeldInputFile(std::shared_ptr<InputFile> inner) : inner(std::move(inner)) {}
  size_t size() override { return inner->size(); }
  bool holdsFree() const override { return false; }
  using InputFile::acquire;
  HeldRange acquire(ByteRange range) override {
    if (IoContext::current()) {
      acquireTotals.ranges.fetch_add(1, std::memory_order_relaxed);
      acquireTotals.bytes.fetch_add(range.length, std::memory_order_relaxed);
    }
    return inner->acquire(range);
  }
};

class HeldDirectory : public DelegatingDirectory {
public:
  using DelegatingDirectory::DelegatingDirectory;
  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false, uint64_t size = 0) override {
    auto file = delegate->openFile(name, expectSynced, size);
    return file ? std::make_shared<HeldInputFile>(std::move(file)) : file;
  }
};

class HeldLayer : public DelegatingDirFactory {
public:
  using DelegatingDirFactory::DelegatingDirFactory;
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return std::make_shared<HeldDirectory>(delegate->directory(root, path, std::move(options)));
  }
  std::string describe() const override { return "io_held(" + delegate->describe() + ")"; }
};

// Registered before the registries freeze at test startup.
const bool heldLayerRegistered = [] {
  StorageRegistry::global().addLayer(
      "io_held", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) -> std::unique_ptr<DirectoryFactory> {
        return std::make_unique<HeldLayer>(std::move(inner));
      });
  return true;
}();

IoContext context(uint64_t queryId, IoCounters* counters = nullptr) {
  IoContext io;
  io.queryId = queryId;
  io.counters = counters;
  return io;
}

constexpr size_t kTasks = 8;

// Runs one published task per slot; each records the context it sees and
// counts a 100-byte acquire.
std::vector<std::optional<IoContext>> seenInTasks() {
  std::vector<std::optional<IoContext>> seen(kTasks);
  FanoutScope runner(nullptr, FanoutPolicy::EVERY);
  task_group_fanout(&runner, kTasks, [&](size_t i) {
    return [&seen, i] {
      seen[i] = captureIoContext();
      countAcquire(100);
    };
  });
  runner.join();
  return seen;
}

}  // namespace

TEST(IoContextTest, tasksSeeTheSpawnersContext) {
  IoCounters counters;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
  IoContext io = context(7, &counters);
  io.requestId = "r7";
  io.deadline = deadline;
  io.fillCache = false;
  std::vector<std::optional<IoContext>> seen;
  {
    IoScope scope(&io);
    seen = seenInTasks();
  }
  EXPECT_EQ(nullptr, IoContext::current());
  for (const auto& task : seen) {
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(7u, task->queryId);
    EXPECT_EQ("r7", task->requestId);
    EXPECT_EQ(deadline, task->deadline);
    EXPECT_FALSE(task->fillCache);
    EXPECT_EQ(&counters, task->counters);
  }
  auto totals = counters.snapshot();
  EXPECT_EQ(kTasks, totals.acquires);
  EXPECT_EQ(kTasks * 100, totals.acquiredBytes);
  EXPECT_EQ(0u, totals.requests);

  // The same worker threads, now without a context: nothing leaked to them.
  for (const auto& task : seenInTasks()) EXPECT_FALSE(task.has_value());
  EXPECT_EQ(kTasks, counters.snapshot().acquires);
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
  std::stop_source cancel;
  io.stop = cancel.get_token();
  auto shared = io.shared();
  EXPECT_EQ(3u, shared.queryId);
  EXPECT_EQ(&counters, shared.counters);
  EXPECT_FALSE(shared.hasDeadline());
  EXPECT_TRUE(shared.fillCache);
  EXPECT_FALSE(shared.stop.stop_possible());
}

// Storage waits end a margin before the transport deadline, so their error
// reaches a client that still waits: 5% of the budget, at most 500 ms.
TEST(IoContextTest, waitsEndAMarginBeforeTheDeadline) {
  using namespace std::chrono_literals;
  auto now = IoContext::Clock::now();
  IoContext io;
  EXPECT_EQ(IoContext::Clock::time_point::max(), io.waitDeadline());
  io.setDeadline(now + 2s, now);
  EXPECT_EQ(now + 2s - 100ms, io.waitDeadline());
  io.setDeadline(now + 30s, now);
  EXPECT_EQ(now + 30s - 500ms, io.waitDeadline());
  EXPECT_EQ(now + 30s, io.deadline);
  io.setDeadline(now - 1s, now);  // already passed: no margin to keep
  EXPECT_EQ(now - 1s, io.waitDeadline());
  io.setDeadline(now + 30s, now);
  EXPECT_EQ(IoContext::Clock::time_point::max(), io.shared().waitDeadline());
}

// A search over storage that hands out held ranges reports the ranges its
// segments acquired, exactly as the storage saw them under its context. A
// repeat finds every range held and acquires none.
TEST(IoContextTest, searchProfileCountsAcquires) {
  using namespace luxir::test;
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"io_held"};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  std::vector<Doc> docs;
  for (int i = 0; i < 100; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "text_un", (i == 50 ? "needle hay " : "hay ") + std::to_string(i)));
  }
  ASSERT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
  auto search = [&] {
    auto req = localReq(h.getSearchEngine());
    req->collection("main").profile().topDocs("q").matchQuery("text_un", "needle").fields({"id", "text_un"});
    req->execute(true);
    EXPECT_OK(req);
    return req->responses.back()->proto.profile.value_or(api::ExecutionProfile{});
  };
  auto metric = [&] { return node.metrics->counts().counters[(size_t)MetricCounter::SEARCH_IO_ACQUIRES]; };
  auto ranges = acquireTotals.ranges.load();
  auto bytes = acquireTotals.bytes.load();
  auto metricBefore = metric();
  auto cold = search();
  EXPECT_GT(cold.io_acquires, 0u);
  EXPECT_EQ(acquireTotals.ranges.load() - ranges, cold.io_acquires);
  EXPECT_EQ(acquireTotals.bytes.load() - bytes, cold.io_acquired_bytes);
  EXPECT_EQ(0u, cold.io_requests);  // this storage issues no store requests
  EXPECT_EQ(metricBefore + cold.io_acquires, metric());

  auto warm = search();
  EXPECT_EQ(0u, warm.io_acquires);
  EXPECT_EQ(0u, warm.io_acquired_bytes);
}
