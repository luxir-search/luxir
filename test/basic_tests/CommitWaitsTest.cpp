// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "test/LuxirTest.h"
#include <future>
#include "test/CollectionHelper.h"
#include "luxir/server/CommitWaits.h"

namespace luxir::test {
using namespace std::chrono_literals;
using Outcome = api::ReplicaResult::Outcome;

class CommitWaitsTest : public LuxirTest {};

TEST_F(CommitWaitsTest, satisfiedFloorRetainsOriginalCollectionAcrossReplacement) {
  LuxirNode node(LuxirConfig{});
  auto original = node.getCollection("main");
  auto id = original->getShard()->getSnapshots().snapshot()->id;
  id.index_gen++;
  std::optional<CommitWaits::CommitResult> result;
  auto& waits = node.getCommitWaits();
  waits.awaitCommit(CollectionId::of("main"), id, waits.deadlineAfter(30000), {}, [&](auto value) { result = std::move(value); });
  ASSERT_FALSE(result);
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(result);
  ASSERT_FALSE(result->error);
  node.deleteCollection("main");
  auto replacement = node.createCollection("main");
  EXPECT_EQ(original, result->collection);
  EXPECT_NE(replacement, result->collection);
}

TEST_F(CommitWaitsTest, injectedClockDrivesFloorsReplicasAndCapturedLiveness) {
  LuxirNode node(LuxirConfig{});
  auto collection = node.getCollection("main");
  auto id = collection->getShard()->getSnapshots().snapshot()->id;
  auto start = ReplicationSource::Clock::now();
  std::atomic<int> elapsed{0};
  auto now = [&] { return start + std::chrono::seconds(elapsed.load()); };
  ReplicationSource source(10s, now);
  CommitWaits waits(source, true, now);
  source.onAcknowledgmentsChanged([&](const CollectionId* id) { waits.acknowledged(id); });
  source.registered(CollectionId::of("main"), collection); waits.registered(CollectionId::of("main"), collection);
  source.installed({"first", "default", "main", id.token()});
  auto future = id; future.index_gen++;
  std::optional<CommitWaits::CommitResult> floor;
  std::optional<api::ReplicaResult> count, all;
  waits.awaitCommit(CollectionId::of("main"), future, waits.deadlineAfter(2000), {}, [&](auto result) { floor = std::move(result); });
  waits.awaitReplicas(CollectionId::of("main"), id, {uint32_t{2}}, waits.deadlineAfter(2000), {}, [&](auto result) { count = result; });
  waits.awaitReplicas(CollectionId::of("main"), future, {api::AllReplicas{}}, waits.deadlineAfter(30000), {}, [&](auto result) { all = result; });
  EXPECT_FALSE(floor); EXPECT_FALSE(count); EXPECT_FALSE(all);
  elapsed = 2; waits.poll();
  ASSERT_TRUE(floor); EXPECT_EQ("stale_replica", floor->error->code);
  ASSERT_TRUE(count); EXPECT_EQ(Outcome::TIMED_OUT, count->outcome);
  EXPECT_FALSE(all);
  elapsed = 10; source.expire();
  ASSERT_TRUE(all); EXPECT_EQ(Outcome::SATISFIED, all->outcome); EXPECT_EQ(0u, all->wanted);
}

TEST_F(CommitWaitsTest, cancellationAndRemovalPreserveOwningCompletion) {
  LuxirNode node(LuxirConfig{});
  auto id = node.getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  for (bool remove : {false, true}) {
    struct Request { api::UpdateRequest proto; };
    auto request = std::make_shared<Request>();
    request->proto.commit.emplace().wait_for_replicas = api::ReplicaRequirement{uint32_t{1}};
    request->proto.request_id = "request";
    std::weak_ptr<Request> body = request;
    auto message = std::make_unique<ProtoUpdateMessage>(&request->proto);
    message->resultingCommit = id;
    message->addId("owned-id");
    auto completed = message->takeCompletion(node);
    message.reset(); request.reset();
    EXPECT_TRUE(body.expired());
    std::stop_source stop;
    std::shared_ptr<ProtoUpdateMessage::Completion> result;
    completed->await(node, [&](auto value) { EXPECT_TRUE(body.expired()); result = std::move(value); }, stop.get_token());
    EXPECT_FALSE(result);
    if (remove) node.deleteCollection("main"); else stop.request_stop();
    ASSERT_TRUE(result);
    EXPECT_EQ(id.token(), result->response.commit);
    EXPECT_EQ(Outcome::CANCELLED, result->response.replicas->outcome);
    EXPECT_EQ(api::UpdateResponse::Status::OK, result->response.status);
    EXPECT_FALSE(result->response.error);
    ASSERT_EQ(1u, result->response.ids.size()); EXPECT_EQ("owned-id", result->response.ids.front());
    EXPECT_EQ("request", result->response.request_id);
  }
}

TEST_F(CommitWaitsTest, oldIdentityCannotOverwriteCatalogOrSatisfyFloor) {
  LuxirNode node(LuxirConfig{});
  auto original = node.getCollection("main");
  auto id = original->getShard()->getSnapshots().snapshot()->id;
  node.deleteCollection("main");
  auto replacement = node.createCollection("main");
  auto current = replacement->getShard()->getSnapshots().snapshot()->id;
  auto& source = node.getReplication();
  source.updated(CollectionId::of("main"), *original);
  node.getCommitWaits().updated(CollectionId::of("main"), *original);
  std::pmr::monotonic_buffer_resource arena;
  EXPECT_EQ(current.token(), source.catalog(arena).collections.front().commit);
  std::optional<CommitWaits::CommitResult> result;
  node.getCommitWaits().awaitCommit(CollectionId::of("main"), id, node.getCommitWaits().deadlineAfter(30000), {},
      [&](auto value) { result = std::move(value); });
  ASSERT_TRUE(result); ASSERT_TRUE(result->error);
  EXPECT_EQ("commit_incarnation_mismatch", result->error->code);
  EXPECT_FALSE(result->collection);
}
TEST_F(CommitWaitsTest, allMembershipIsCapturedAtLocalCompletion) {
  LuxirNode node(LuxirConfig{});
  CollectionHelper h(node, "main");
  auto old = h.collection().getShard()->getSnapshots().snapshot()->id;
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  api::UpdateRequest request;
  request.commit.emplace().wait_for_replicas = api::ReplicaRequirement{api::AllReplicas{}};
  auto message = std::make_unique<ProtoUpdateMessage>(&request);
  message->resultingCommit = h.collection().getShard()->getSnapshots().snapshot()->id;
  auto completed = message->takeCompletion(node);
  message.reset();
  node.getReplication().installed({"late", "default", "main", old.token()});
  bool delivered = false;
  completed->await(node, [&](auto result) {
    delivered = true;
    EXPECT_EQ(Outcome::SATISFIED, result->response.replicas->outcome);
    EXPECT_EQ(0u, result->response.replicas->wanted);
  });
  EXPECT_TRUE(delivered);
}

TEST_F(CommitWaitsTest, allExpiryCompletesWithoutHttpMaintenance) {
  LuxirNode node(LuxirConfig{});
  auto collection = node.getCollection("main");
  auto id = collection->getShard()->getSnapshots().snapshot()->id;
  ReplicationSource source(30ms);
  CommitWaits waits(source, false);
  source.registered(CollectionId::of("main"), collection); waits.registered(CollectionId::of("main"), collection);
  source.installed({"f", "default", "main", id.token()});
  id.index_gen++;
  std::promise<api::ReplicaResult> completed;
  waits.awaitReplicas(CollectionId::of("main"), id, {api::AllReplicas{}}, waits.deadlineAfter(30000), {},
      [&](auto result) { completed.set_value(result); });
  auto result = completed.get_future();
  ASSERT_EQ(std::future_status::ready, result.wait_for(2s));
  EXPECT_EQ(Outcome::SATISFIED, result.get().outcome);
}

}
