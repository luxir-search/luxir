// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/index/IndexWriter.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {
namespace {

using namespace std::chrono_literals;

bool waitForIds(CollectionHelper& helper, size_t count, std::chrono::milliseconds limit) {
  auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (allIds(helper).size() == count) return true;
    std::this_thread::sleep_for(10ms);
  }
  return allIds(helper).size() == count;
}

IndexResult addWithin(CollectionHelper& helper, const std::string& id, uint64_t withinMs) {
  CollectionHelper::UpdateBuilder request;
  request.add(flatdoc("id", id, "text_w", "commit within"));
  request.commitWithin(withinMs);
  return helper.submit(request);
}

}  // namespace

class CommitWithinTest : public LuxirTest {};

TEST_F(CommitWithinTest, defersAndCommitsByDeadline) {
  CollectionHelper helper("main");
  // The response must not wait for the deferred commit; visibility follows.
  ASSERT_TRUE(addWithin(helper, "d1", 100).success);
  EXPECT_TRUE(waitForIds(helper, 1, 10s));
}

TEST_F(CommitWithinTest, coalescesToOneCommitPerWindow) {
  CollectionHelper helper("main");
  auto writer = helper.getIndexWriter();
  uint64_t before = writer->stats(false).commits;
  // No hour-long window closes during the test, so the last update's short window
  // is the one deadline, and its commit covers every pending update.
  constexpr size_t docs = 20;
  for (size_t i = 0; i + 1 < docs; i++) {
    ASSERT_TRUE(addWithin(helper, "d" + std::to_string(i), 3'600'000).success);
  }
  ASSERT_TRUE(addWithin(helper, "d" + std::to_string(docs - 1), 1).success);
  EXPECT_TRUE(waitForIds(helper, docs, 10s));
  EXPECT_EQ(before + 1, writer->stats(false).commits);
}

TEST_F(CommitWithinTest, explicitCommitSatisfiesPendingDeadline) {
  CollectionHelper helper("main");
  auto writer = helper.getIndexWriter();
  // A window beyond the steady clock's representable range: its deadline must
  // saturate (an overflow would wrap it into the past and commit at once), and
  // the deadline must never be what makes this visible.
  ASSERT_TRUE(addWithin(helper, "d1", (uint64_t)1 << 62).success);
  {
    std::lock_guard lock(writer->autoCommitMutex);
    EXPECT_GT(writer->autoCommitDeadline, DeadlineScheduler::Clock::now() + std::chrono::hours(24 * 365));
  }
  uint64_t before = writer->stats(false).commits;
  writer->commit();
  EXPECT_EQ((std::vector<std::string>{"d1"}), allIds(helper));
  EXPECT_EQ(before + 1, writer->stats(false).commits);
  // The satisfied deadline leaves nothing for a trailing auto-commit to do.
  std::lock_guard lock(writer->autoCommitMutex);
  EXPECT_FALSE(writer->autoCommitPending);
}

}  // namespace luxir::test
