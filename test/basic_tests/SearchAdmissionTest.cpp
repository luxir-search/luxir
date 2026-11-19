// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <stop_token>
#include <string>
#include <thread>

#include "luxir/search/SearchAdmission.h"
#include "luxir/search/SearchEngine.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"

#include <future>

using namespace luxir;
using namespace luxir::test;
using Refusal = SearchAdmission::Refusal;

namespace {

SearchAdmission::Limits limits(uint32_t running, uint32_t perCollection, uint32_t queued,
                               std::chrono::milliseconds wait = std::chrono::milliseconds(0)) {
  return {.maxRunning = running, .maxRunningPerCollection = perCollection, .maxRunningRemote = running,
          .maxQueued = queued, .maxQueueWait = wait};
}

// Records which searches started or were refused; a started one keeps its slot.
struct Searches {
  std::map<std::string, SearchAdmission::Slot> slots;
  std::string log;

  void admit(SearchAdmission& gate, std::string collection, std::string id, std::stop_token stop = {},
             bool remote = false) {
    gate.admit(collection, remote, stop,
        [this, id](SearchAdmission::Slot slot) { slots[id] = std::move(slot); log += "start" + id + " "; },
        [this, id](Refusal why) {
          static const char* names[] = {"full", "timeout", "cancel", "shutdown"};
          log += std::string(names[(int)why]) + id + " ";
        });
  }
  void finish(const std::string& id) { slots.at(id).reset(); }
  // Releasing a slot can start a waiter, which adds a slot: drain one by one.
  ~Searches() {
    while (!slots.empty()) slots.extract(slots.begin()).mapped().reset();
  }
};

}  // namespace

TEST(SearchAdmissionTest, queuesInOrderAndHonoursCancellation) {
  SearchAdmission gate(limits(1, 1, 8));
  Searches s;
  std::stop_source stop1, stop2;
  s.admit(gate, "a", "0");
  s.admit(gate, "a", "1", stop1.get_token());
  s.admit(gate, "a", "2", stop2.get_token());
  EXPECT_EQ("start0 ", s.log);
  EXPECT_EQ(2u, gate.snapshot().queued);

  stop1.request_stop();  // leaves the queue without starting
  s.finish("0");         // the slot passes to the next waiter
  stop2.request_stop();  // already started: nothing to cancel
  s.finish("2");
  EXPECT_EQ("start0 cancel1 start2 ", s.log);

  auto stats = gate.snapshot();
  EXPECT_EQ(0u, stats.running);
  EXPECT_EQ(0u, stats.queued);
  EXPECT_EQ(2u, stats.admitted);
  EXPECT_EQ(1u, stats.waited);
  EXPECT_EQ(1u, stats.cancelled);
  EXPECT_TRUE(stats.collections.empty());

  // A stop requested before the search queues cancels it at once.
  std::stop_source stopped;
  stopped.request_stop();
  s.admit(gate, "a", "3");
  s.admit(gate, "a", "4", stopped.get_token());
  EXPECT_EQ("start0 cancel1 start2 start3 cancel4 ", s.log);
}

// One collection at its cap leaves the remaining slot to another collection.
TEST(SearchAdmissionTest, perCollectionCapLeavesASlotForOthers) {
  SearchAdmission gate(limits(2, 1, 8));
  Searches s;
  s.admit(gate, "busy", "0");
  s.admit(gate, "busy", "1");  // at its cap: queues though a slot is free
  s.admit(gate, "other", "2");
  EXPECT_EQ("start0 start2 ", s.log);
  auto stats = gate.snapshot();
  ASSERT_EQ(2u, stats.collections.size());
  EXPECT_EQ("busy", stats.collections[0].collection);
  EXPECT_EQ(1u, stats.collections[0].running);
  EXPECT_EQ(1u, stats.collections[0].queued);
  // The inline lane needs an empty queue, whatever the collection.
  EXPECT_FALSE(gate.tryAcquire("third", false));
  s.finish("0");
  EXPECT_EQ("start0 start2 start1 ", s.log);
}

TEST(SearchAdmissionTest, remoteCapIsASubLimit) {
  SearchAdmission gate({.maxRunning = 3, .maxRunningPerCollection = 3, .maxRunningRemote = 1, .maxQueued = 8});
  Searches s;
  s.admit(gate, "cold", "0", {}, true);
  s.admit(gate, "cold", "1", {}, true);
  s.admit(gate, "hot", "2");
  EXPECT_EQ("start0 start2 ", s.log);
  EXPECT_EQ(1u, gate.snapshot().runningRemote);
  s.finish("0");
  EXPECT_EQ("start0 start2 start1 ", s.log);
}

TEST(SearchAdmissionTest, fullQueueRefusesOnArrival) {
  SearchAdmission gate(limits(1, 1, 1));
  Searches s;
  s.admit(gate, "a", "0");
  s.admit(gate, "a", "1");
  s.admit(gate, "b", "2");
  EXPECT_EQ("start0 full2 ", s.log);
  EXPECT_EQ(1u, gate.snapshot().rejectedFull);
  // With no queue at all, a search runs only when a slot is free.
  SearchAdmission none(limits(1, 1, 0));
  Searches t;
  t.admit(none, "a", "0");
  t.admit(none, "a", "1");
  EXPECT_EQ("start0 full1 ", t.log);
}

TEST(SearchAdmissionTest, queueTimeoutRefusesTheWaiter) {
  SearchAdmission gate(limits(1, 1, 8, std::chrono::milliseconds(20)));
  Searches s;
  s.admit(gate, "a", "0");
  s.admit(gate, "a", "1");
  for (int i = 0; i < 500 && gate.snapshot().rejectedTimeout == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ("start0 timeout1 ", s.log);
  EXPECT_EQ(0u, gate.snapshot().queued);
  s.finish("0");  // the running search was never timed out
  EXPECT_EQ(0u, gate.snapshot().running);
}

// Freed slots go to the collections in turn, not to the longest backlog.
TEST(SearchAdmissionTest, servesCollectionsRoundRobin) {
  SearchAdmission gate(limits(1, 1, 8));
  Searches s;
  s.admit(gate, "a", "0");
  s.admit(gate, "a", "a1");
  s.admit(gate, "a", "a2");
  s.admit(gate, "b", "b1");
  s.admit(gate, "c", "c1");
  s.log.clear();
  for (auto id : {"0", "a1", "b1", "c1", "a2"}) s.finish(id);
  EXPECT_EQ("starta1 startb1 startc1 starta2 ", s.log);
}

// A resume is never refused and is served before any queued search.
TEST(SearchAdmissionTest, resumesTakePriorityAndAreNeverRefused) {
  SearchAdmission gate(limits(1, 1, 0));
  Searches s;
  s.admit(gate, "a", "0");
  SearchAdmission::Slot resumed;
  bool waited = false;
  gate.resume([&](SearchAdmission::Slot slot) { waited = slot.waited(); resumed = std::move(slot); });
  EXPECT_FALSE(resumed);
  EXPECT_EQ(1u, gate.snapshot().resumesQueued);
  s.finish("0");
  EXPECT_TRUE(resumed);
  EXPECT_TRUE(waited);
  resumed.reset();
  // A free slot runs the resume on the calling thread.
  bool ran = false;
  gate.resume([&](SearchAdmission::Slot slot) { ran = (bool)slot && !slot.waited(); });
  EXPECT_TRUE(ran);
  EXPECT_EQ(0u, gate.snapshot().running);
  EXPECT_EQ(2u, gate.snapshot().resumes);
}

// A throwing start still frees its slot.
TEST(SearchAdmissionTest, throwingStartReleasesTheSlot) {
  SearchAdmission gate(limits(1, 1, 8));
  gate.admit("a", false, {}, [](SearchAdmission::Slot) { throw std::runtime_error("boom"); }, [](Refusal) {});
  EXPECT_EQ(0u, gate.snapshot().running);
  EXPECT_TRUE(gate.tryAcquire("a", false));
}

TEST(SearchAdmissionTest, crowdedWhenOthersRunOrWait) {
  SearchAdmission gate(limits(2, 2, 8));
  auto mine = gate.tryAcquire("a", false);
  EXPECT_FALSE(gate.crowded(2));  // alone: may use every slot
  auto other = gate.tryAcquire("b", false);
  EXPECT_TRUE(gate.crowded(2));
  EXPECT_FALSE(gate.crowded(1));
}

// --- End to end, through SearchEngine::dispatch ---------------------------

namespace {

class SearchAdmissionEngineTest : public LuxirTest {};

// A LocalReq whose final reply is a completion latch (reply() runs before
// set_value, so the future orders `responses` for the test thread).
class AdmittedReq : public LocalReq {
public:
  std::promise<void> finished;
  using LocalReq::LocalReq;
  ReplyStatus reply(SearchResponse& response) override {
    ReplyStatus status = LocalReq::reply(response);
    if (response.last) finished.set_value();
    return status;
  }
};

struct Dispatched {
  LocalReqHandle req;
  std::future<void> done;
  bool finished(std::chrono::seconds wait = std::chrono::seconds(60)) {
    return done.wait_for(wait) == std::future_status::ready;
  }
};

Dispatched dispatch(SearchEngine& engine, std::string_view collection, int32_t maxParallel = 0,
                    SearchEngine::Receiver receiver = SearchEngine::Receiver::OFFLOAD) {
  auto* arena = createArena();
  auto* req = luxir::arenaCreate<AdmittedReq>(*arena, engine, *arena);
  Dispatched d{LocalReqHandle(req), req->finished.get_future()};
  req->collection(collection).topDocs().allQuery();
  engine.dispatch(*req, maxParallel, receiver);
  return d;
}

// Holds the first search that reaches its ops until released.
class Stall {
  std::promise<void> reached, released;
  std::shared_future<void> releasedFuture = released.get_future().share();
  std::atomic<bool> first{true};

public:
  Stall() {
    Signal::listen("searchRootChildStart", [this](void*, void*, void*) -> void* {
      if (first.exchange(false)) {
        reached.set_value();
        releasedFuture.wait();
      }
      return nullptr;
    });
  }
  ~Stall() { Signal::unlisten("searchRootChildStart"); }
  bool awaitStall() { return reached.get_future().wait_for(std::chrono::seconds(60)) == std::future_status::ready; }
  void release() { released.set_value(); }
};

LuxirConfig admissionConfig(int running, int perCollection, int queued, int64_t queueMs = 0) {
  LuxirConfig config;
  config.server.workers = 2;
  config.server.http.threads = 1;
  config.search.max_running = running;
  config.search.max_running_per_collection = perCollection;
  config.search.max_queued = queued;
  config.search.max_queue_ms = queueMs;
  return config;
}

bool eventually(const std::function<bool()>& condition) {
  for (int i = 0; i < 6000 && !condition(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return condition();
}

}  // namespace

// Long searches on one collection never take the last slot: another
// collection's search runs while the first collection's backlog waits.
TEST_F(SearchAdmissionEngineTest, perCollectionCapKeepsOtherCollectionsRunnable) {
  LuxirNode node(admissionConfig(2, 1, 8));
  CollectionHelper a(node, "a"), b(node, "b");
  ASSERT_TRUE(a.indexAll({flatdoc("id", "1")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(b.indexAll({flatdoc("id", "2")}, UpdateMessage::COMMIT).success);
  auto& engine = node.getSearchEngine();
  Stall stall;
  auto a1 = dispatch(engine, "a");
  ASSERT_TRUE(stall.awaitStall());
  auto a2 = dispatch(engine, "a");
  EXPECT_EQ(1u, engine.getAdmission().snapshot().queued);
  auto b1 = dispatch(engine, "b");
  ASSERT_TRUE(b1.finished());
  EXPECT_TRUE(b1.req->ok()) << b1.req->errorMsg();
  EXPECT_FALSE(a2.finished(std::chrono::seconds(0)));
  stall.release();
  ASSERT_TRUE(a1.finished());
  ASSERT_TRUE(a2.finished());
  EXPECT_TRUE(a2.req->ok()) << a2.req->errorMsg();
  auto stats = engine.getAdmission().snapshot();
  EXPECT_EQ(0u, stats.running);
  EXPECT_EQ(3u, stats.admitted);
  EXPECT_EQ(1u, stats.waited);
}

// A full queue refuses on arrival, a wait past search.max-queue-ms refuses the
// waiter; both are RESOURCE_EXHAUSTED and count as rejected searches.
TEST_F(SearchAdmissionEngineTest, overflowAndQueueTimeoutAreRejected) {
  LuxirNode node(admissionConfig(1, 1, 1, 50));
  CollectionHelper a(node, "a");
  ASSERT_TRUE(a.indexAll({flatdoc("id", "1")}, UpdateMessage::COMMIT).success);
  auto& engine = node.getSearchEngine();
  Stall stall;
  auto running = dispatch(engine, "a");
  ASSERT_TRUE(stall.awaitStall());
  auto queued = dispatch(engine, "a");
  auto overflow = dispatch(engine, "a");
  ASSERT_TRUE(overflow.finished());
  EXPECT_EQ(ErrorKind::RESOURCE_EXHAUSTED, overflow.req->errorKind());
  EXPECT_EQ("search_queue_full", overflow.req->errorCode());
  ASSERT_TRUE(queued.finished());
  EXPECT_EQ("search_queue_timeout", queued.req->errorCode());
  stall.release();
  ASSERT_TRUE(running.finished());
  EXPECT_TRUE(running.req->ok());
  auto counts = node.metrics->counts();
  EXPECT_EQ(2u, counts.activities[metricActivity(MetricStage::SEARCH)].completed[(size_t)MetricOutcome::REJECTED]);
  auto stats = engine.getAdmission().snapshot();
  EXPECT_EQ(1u, stats.rejectedFull);
  EXPECT_EQ(1u, stats.rejectedTimeout);
}

// A search that fails while it holds its slot frees it, inline or offloaded.
TEST_F(SearchAdmissionEngineTest, failedSearchesReleaseTheirSlot) {
  LuxirNode node(admissionConfig(1, 1, 0));
  CollectionHelper a(node, "a");
  ASSERT_TRUE(a.indexAll({flatdoc("id", "1")}, UpdateMessage::COMMIT).success);
  auto& engine = node.getSearchEngine();
  for (auto receiver : {SearchEngine::Receiver::MAY_EXECUTE, SearchEngine::Receiver::OFFLOAD}) {
    auto failed = dispatch(engine, "a", 2, receiver);  // invalid max_parallel: fails in prepare()
    ASSERT_TRUE(failed.finished());
    EXPECT_FALSE(failed.req->ok());
    auto missing = dispatch(engine, "no_such_collection", 0, receiver);
    ASSERT_TRUE(missing.finished());
    EXPECT_FALSE(missing.req->ok());
    EXPECT_EQ(0u, engine.getAdmission().snapshot().running);
  }
  auto next = dispatch(engine, "a");
  ASSERT_TRUE(next.finished());
  EXPECT_TRUE(next.req->ok()) << next.req->errorMsg();
}

// max_parallel=-1 fans out adaptively while another search holds a slot.
TEST_F(SearchAdmissionEngineTest, everySiblingDowngradesWhenCrowded) {
  LuxirNode node(admissionConfig(2, 2, 8));
  CollectionHelper a(node, "a");
  ASSERT_TRUE(a.indexAll({flatdoc("id", "1")}, UpdateMessage::COMMIT).success);
  auto& engine = node.getSearchEngine();
  auto alone = dispatch(engine, "a", -1);
  ASSERT_TRUE(alone.finished());
  EXPECT_EQ(0u, engine.getAdmission().snapshot().downgraded);
  Stall stall;
  auto busy = dispatch(engine, "a", 1);
  ASSERT_TRUE(stall.awaitStall());
  auto crowded = dispatch(engine, "a", -1);
  ASSERT_TRUE(crowded.finished());
  EXPECT_TRUE(crowded.req->ok()) << crowded.req->errorMsg();
  EXPECT_EQ(1u, engine.getAdmission().snapshot().downgraded);
  stall.release();
  ASSERT_TRUE(busy.finished());
}

// Over HTTP: with every slot taken and no queue, a search is refused with 429
// and Retry-After, while a paused stream's resumed bursts are never refused.
TEST_F(SearchAdmissionEngineTest, httpRefusesWith429AndResumesAreNeverRefused) {
  LuxirNode node(admissionConfig(1, 1, 0));
  CollectionHelper helper(node, "main");
  std::vector<Doc> docs;
  for (int i = 0; i < 500; i++) docs.push_back(flatdoc("id", "d" + std::to_string(i), "pad_s", std::string(400, 'x')));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  HttpServer server(node, 1, 0, /*streamBufferBytes=*/4096);
  server.start();
  int port = server.getPort();
  {
    Stall stall;
    // Off the shard (max_parallel 1), so the shard can read the next request.
    auto held = std::async(std::launch::async, [port] {
      return httpRequest(port, http::verb::post, "/collections/main/_search", R"({"query":{"all":true},"max_parallel":1})");
    });
    ASSERT_TRUE(stall.awaitStall());
    auto refused = httpRequest(port, http::verb::post, "/collections/main/_search", R"({"query":{"all":true}})");
    EXPECT_EQ(429, refused.result_int()) << refused.body();
    EXPECT_EQ("1", std::string(refused[http::field::retry_after]));
    EXPECT_NE(std::string::npos, refused.body().find("search_queue_full")) << refused.body();
    stall.release();
    EXPECT_EQ(200, held.get().result_int());
  }
  // Each pause re-admits through the resume lane, which has no queue limit.
  int64_t pausesBefore = streamPauseCount.load();
  auto streamed = httpRequest(port, http::verb::post, "/collections/main/_search",
      R"({"query":{"all":true},"limit":-1,"batch_size":50,"fields":["id","pad_s"]})");
  EXPECT_EQ(200, streamed.result_int());
  EXPECT_GT(streamPauseCount.load(), pausesBefore);
  size_t count = 0;
  for (size_t pos = 0; (pos = streamed.body().find(R"("id":"d)", pos)) != std::string::npos; pos++) count++;
  EXPECT_EQ(500u, count);
  EXPECT_TRUE(eventually([&] { return node.getSearchEngine().getAdmission().snapshot().running == 0; }));
  EXPECT_GT(node.getSearchEngine().getAdmission().snapshot().resumes, 0u);
  server.shutdown();
}
