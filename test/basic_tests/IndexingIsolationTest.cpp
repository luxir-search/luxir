// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Guards that keep indexing off search: the indexing thread cap, the nice
// applied to indexing workers, and merge write pacing.

#include <chrono>
#include <climits>
#include <filesystem>
#include <future>
#include <string>

#include <CLI/CLI.hpp>
#include <gtest/gtest.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "luxir/server/LuxirNode.h"
#include "luxir/util/ByteRateLimiter.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/TestUtils.h"

namespace luxir::test {
namespace {

using namespace std::chrono_literals;

TEST(ByteRateLimiterTest, chargesShareOneTimelineWithoutIdleCredit) {
  ByteRateLimiter limiter(1000);
  auto t0 = ByteRateLimiter::Clock::time_point{} + 1h;
  EXPECT_EQ(t0 + 500ms, limiter.reserve(500, t0));
  EXPECT_EQ(t0 + 1000ms, limiter.reserve(500, t0));  // a second writer queues behind
  EXPECT_EQ(t0 + 5100ms, limiter.reserve(100, t0 + 5s));  // idle time earns no burst

  ByteRateLimiter off;
  EXPECT_FALSE(off.enabled());
  EXPECT_EQ(t0, off.reserve(1 << 30, t0));
}

TEST(IndexingIsolationTest, options) {
  LuxirConfig config;
  CLI::App app;
  config.addOptions(app);
  app.parse("--server.workers 4 --indexing.threads 2 --indexing.nice 7 "
            "--indexing.merge-max-bytes-per-sec 64MB");
  EXPECT_EQ(2, config.index.threads);
  EXPECT_EQ(7, config.index.nice);
  EXPECT_EQ(64u << 20, config.index.merge_max_bytes_per_sec);
  EXPECT_EQ(2, config.resolveIndexThreads());
  config.index.threads = 9;
  EXPECT_EQ(4, config.resolveIndexThreads());  // capped at the workers
  config.index.threads = 0;
  EXPECT_EQ(4, config.resolveIndexThreads());

  LuxirConfig defaults;
  CLI::App bad;
  defaults.addOptions(bad);
  EXPECT_THROW(bad.parse("--indexing.nice 20"), CLI::ValidationError);
}

TEST(IndexingIsolationTest, threadCapSizesIndexArena) {
  LuxirConfig config;
  config.server.workers = 3;
  config.index.threads = 1;
  config.index.nice = 0;
  LuxirNode node(config);
  EXPECT_EQ(1, node.getIndexThreads());
  EXPECT_EQ(0, node.getIndexNice());
  EXPECT_EQ(2, node.getIndexArena().max_concurrency());  // one worker plus the reserved seat
  CollectionHelper helper(node, "main");
  ASSERT_TRUE(helper.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(1u, helper.durableSegmentCount());
}

#if defined(__linux__)
int threadNice() {
  errno = 0;
  int nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
  return errno != 0 ? INT_MIN : nice;
}

// Runs f on a pool worker of `arena` (enqueue, unlike execute, never runs on
// the caller) and returns its result.
int onWorker(oneapi::tbb::task_arena& arena) {
  std::promise<int> nice;
  auto result = nice.get_future();
  arena.enqueue([&] { nice.set_value(threadNice()); });
  return result.get();
}

TEST(IndexingIsolationTest, indexingWorkersRunNiced) {
  LuxirConfig config;
  config.server.workers = 2;
  config.index.nice = 3;
  LuxirNode node(config);
  int base = threadNice();
  if (node.getIndexNice() == 0) GTEST_SKIP() << "restoring a worker's nice is not permitted here";
  EXPECT_EQ(3, node.getIndexNice());
  EXPECT_EQ(std::min(19, base + 3), onWorker(node.getIndexArena()));
  EXPECT_EQ(base, onWorker(node.getTaskArena()));  // restored on leaving
}
#endif

TEST(IndexingIsolationTest, backgroundMergesArePacedForcedMergesAreNot) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-merge-pacing-" + std::to_string(getpid()));
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  config.index.merge_max_bytes_per_sec = 64ULL << 20;
  config.index.merge_factor = 2;  // the second segment starts a background merge
  LuxirNode node(config);
  CollectionHelper helper(node, "main");
  auto writer = helper.getIndexWriter();
  auto& limiter = node.collections().getMergeRateLimiter();
  auto segment = [&](std::string_view prefix) {
    CollectionHelper::UpdateBuilder request;
    for (int i = 0; i < 50; i++) request.add(flatdoc("id", std::string(prefix) + std::to_string(i), "text_w", "pacing"));
    request.commit();
    return helper.submit(request).success;
  };

  ASSERT_TRUE(segment("a"));
  ASSERT_TRUE(segment("b"));
  CollectionHelper::UpdateBuilder wait;
  wait.commit(/*waitForMerges=*/true);
  ASSERT_TRUE(helper.submit(wait).success);
  EXPECT_EQ(1u, helper.durableSegmentCount());
  auto paused = limiter.paused();
  EXPECT_GT(paused, 0ns);

  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(segment("c"));
  CollectionHelper::UpdateBuilder force;
  force.commit(/*waitForMerges=*/true, /*maxSegments=*/1);
  ASSERT_TRUE(helper.submit(force).success);
  EXPECT_EQ(1u, helper.durableSegmentCount());
  EXPECT_EQ(paused, limiter.paused());
}

}
}
