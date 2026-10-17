// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <latch>
#include <thread>

#include <gtest/gtest.h>

#include "luxir/server/LuxirNode.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"

namespace luxir::test {

class TaskArenaTest : public LuxirTest {};

// Every configured worker runs at once, including when there are as many as
// the affinity width (the small-node default).  TBB's default global pool
// supplies only width - 1 workers, which would leave one task waiting.
TEST_F(TaskArenaTest, allConfiguredWorkersRunConcurrently) {
  LuxirConfig config;
  config.server.workers = std::max(2, oneapi::tbb::info::default_concurrency());
  LuxirNode node(config);
  int workers = node.getWorkers();
  ASSERT_EQ(config.server.workers, workers);

  std::atomic<int> started{0};
  std::atomic<int> sawAll{0};
  std::latch finished(workers);
  for (int i = 0; i < workers; i++) {
    node.getTaskArena().enqueue([&] {
      started++;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (started.load() < workers && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
      if (started.load() == workers) sawAll++;
      finished.count_down();
    });
  }
  finished.wait();
  EXPECT_EQ(workers, sawAll.load());
}

// A collection indexes in its node's index arena whichever thread opens it.  The
// test thread runs in its own arena, as a transport thread auto-creating a
// collection or the main thread reopening one runs outside the node's.
TEST_F(TaskArenaTest, indexingRunsInIndexArena) {
  LuxirNode node;
  class Entries : public oneapi::tbb::task_scheduler_observer {
  public:
    std::atomic<int> count{0};
    explicit Entries(oneapi::tbb::task_arena& arena) : oneapi::tbb::task_scheduler_observer(arena) { observe(true); }
    ~Entries() override { observe(false); }
    void on_scheduler_entry(bool) override { count++; }
  } entries(node.getIndexArena());

  CollectionHelper helper(node, "arena_bound");
  helper.indexAll(std::array{flatdoc("id", "1")}, UpdateMessage::COMMIT);
  EXPECT_GT(entries.count.load(), 0);
}

} // namespace luxir::test
