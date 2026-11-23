// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "luxir/util/IoContext.h"
#include "luxir/util/thread.h"

using namespace luxir;

namespace {

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
