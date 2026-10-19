// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Search deadlines (timeout_ms), cancellation, and admission control
// (search.max_concurrent / search.max_queued).

#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <string>

#include "luxir/util/ExecutionBudget.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {

class SearchDeadlineTest : public LuxirTest {};

namespace {

constexpr int kSegments = 4;
constexpr int kDocsPerSegment = 50;
constexpr int kDocs = kSegments * kDocsPerSegment;

// One commit per segment; every doc is in category "a".
void indexSegments(CollectionHelper& helper) {
  for (int seg = 0; seg < kSegments; seg++) {
    for (int i = 0; i < kDocsPerSegment; i++) {
      int id = seg * kDocsPerSegment + i;
      helper.index(flatdoc("id", std::to_string(id), "cat_s", "a", "x_i", id),
                   i + 1 == kDocsPerSegment ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
    }
  }
}

LocalReqHandle countQuery(CollectionHelper& helper) {
  auto req = localReq(helper.getSearchEngine());
  req->topDocs("q").matchQuery("cat_s", "a").getNumber().limit(5);
  return req;
}

// A commit token one generation past the collection's current one: a search
// with it as min_commit waits (holding its admission slot) until the next commit.
std::string nextCommitToken(CollectionHelper& helper) {
  auto result = helper.index(flatdoc("id", "first", "cat_s", "a"), UpdateMessage::COMMIT);
  CommitId next = *result.commit;
  next.index_gen++;
  return next.token();
}

LuxirConfig cappedConfig(int64_t maxConcurrent, int64_t maxQueued) {
  LuxirConfig config;
  config.search.max_concurrent = maxConcurrent;
  config.search.max_queued = maxQueued;
  return config;
}

} // namespace

TEST_F(SearchDeadlineTest, noTimeoutRunsToCompletion) {
  CollectionHelper helper;
  indexSegments(helper);
  auto req = countQuery(helper);
  req->facet("f", "cat_s");
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_FALSE(req->timedOut());
  EXPECT_FALSE(req->hasWarning("timed_out"));
  EXPECT_EQ(kDocs, req->getMatchCount());
}

TEST_F(SearchDeadlineTest, expiredDeadlineAnswersTimedOutNotError) {
  CollectionHelper helper;
  indexSegments(helper);
  for (int32_t maxParallel : {1, -1}) {
    auto req = countQuery(helper);
    req->facet("f", "cat_s");
    // As a gRPC call deadline that has already passed would.
    req->transportDeadline = std::chrono::steady_clock::now();
    req->execute(maxParallel);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_TRUE(req->timedOut());
    EXPECT_TRUE(req->hasWarning("timed_out"));
    EXPECT_EQ(0, req->getMatchCount());
  }
}

TEST_F(SearchDeadlineTest, deadlineMidQueryKeepsPartialResults) {
  CollectionHelper helper;
  indexSegments(helper);
  auto reset = scope_guard([]() noexcept { ExecutionBudget::expireAfterChecksForTests = 0; });
  // Serially, the first segment's start and its first window pass; a later
  // check expires the budget, so the remaining segments collect nothing.
  ExecutionBudget::expireAfterChecksForTests = 3;
  auto req = countQuery(helper);
  req->timeoutMs(60'000).execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_TRUE(req->timedOut());
  EXPECT_TRUE(req->hasWarning("timed_out"));
  EXPECT_GT(req->getMatchCount(), 0);
  EXPECT_LT(req->getMatchCount(), kDocs);
  // What was collected is still returned.
  EXPECT_FALSE(req->getDocs().empty());
}

TEST_F(SearchDeadlineTest, cancelledSearchFails) {
  CollectionHelper helper;
  indexSegments(helper);
  auto req = countQuery(helper);
  req->waitCancellation = std::stop_source{};
  req->waitCancellation.request_stop();
  req->execute(false);
  EXPECT_FALSE(req->ok());
  EXPECT_EQ(ErrorKind::CANCELLED, req->errorKind());
  EXPECT_EQ("cancelled", req->errorCode());
}

TEST_F(SearchDeadlineTest, admissionRejectsBeyondQueue) {
  LuxirNode node(cappedConfig(1, 0));
  CollectionHelper helper(node);
  auto blocking = localReq(helper.getSearchEngine());
  blocking->minCommit(nextCommitToken(helper), 200).topDocs("q").allQuery();
  blocking->dispatch();
  EXPECT_FALSE(blocking->isDone());

  auto rejected = countQuery(helper);
  rejected->dispatch();
  ASSERT_TRUE(rejected->isDone());
  EXPECT_EQ(ErrorKind::RESOURCE_EXHAUSTED, rejected->errorKind());
  EXPECT_EQ("search_queue_full", rejected->errorCode());
  blocking->waitDone();
}

TEST_F(SearchDeadlineTest, queuedSearchRunsWhenSlotFrees) {
  LuxirNode node(cappedConfig(1, 1));
  CollectionHelper helper(node);
  auto blocking = localReq(helper.getSearchEngine());
  blocking->minCommit(nextCommitToken(helper), 30'000).topDocs("q").allQuery();
  blocking->dispatch();

  auto queued = countQuery(helper);
  queued->dispatch();
  EXPECT_FALSE(queued->isDone());
  auto rejected = countQuery(helper);
  rejected->dispatch();
  EXPECT_EQ("search_queue_full", rejected->errorCode());

  helper.index(flatdoc("id", "second", "cat_s", "a"), UpdateMessage::COMMIT);
  blocking->waitDone();
  queued->waitDone();
  ASSERT_TRUE(blocking->ok()) << blocking->errorMsg();
  ASSERT_TRUE(queued->ok()) << queued->errorMsg();
  EXPECT_FALSE(queued->timedOut());
  EXPECT_EQ(2, queued->getMatchCount());
}

TEST_F(SearchDeadlineTest, queuedPastDeadlineAnswersTimedOutWithoutRunning) {
  LuxirNode node(cappedConfig(1, 1));
  CollectionHelper helper(node);
  auto blocking = localReq(helper.getSearchEngine());
  blocking->minCommit(nextCommitToken(helper), 100).topDocs("q").allQuery();
  blocking->dispatch();

  auto queued = countQuery(helper);
  queued->timeoutMs(1).dispatch();
  blocking->waitDone();
  queued->waitDone();
  ASSERT_TRUE(queued->ok()) << queued->errorMsg();
  EXPECT_TRUE(queued->timedOut());
  EXPECT_TRUE(queued->hasWarning("timed_out"));
  EXPECT_EQ(nullptr, queued->docList("q"));
}

} // namespace luxir::test
