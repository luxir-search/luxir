// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <CLI/CLI.hpp>
#include <array>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "luxir/LuxirConfig.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/util/LoadSignal.h"
#include "test/TaskSpawnCounter.h"
#include "test/TestUtils.h"

namespace luxir::test {

TEST(LoadSignalTest, nestedScopesOnlyClearAtOutermostEnd) {
  TaskClockGuard clock(0);
  LoadRegistry registry({2, 4});
  LoadRegistry::Registration owner(registry), reader(registry);
  auto reads = taskClockReadsForTests.load();
  {
    LoadScope outer(owner, LoadKind::REQUEST);
    { LoadScope inner(owner, LoadKind::SHARE); }
    EXPECT_EQ(reads, taskClockReadsForTests.load());
    EXPECT_EQ((std::array<uint32_t, 4>{1, 1, 1, 1}), reader.snapshot());
    EXPECT_EQ((std::array<uint32_t, 4>{0, 0, 0, 0}), owner.snapshot());
    reads = taskClockReadsForTests.load();
  }
  EXPECT_EQ(reads + 1, taskClockReadsForTests.load());
  EXPECT_EQ((std::array<uint32_t, 4>{0, 1, 1, 1}), reader.snapshot());
}

TEST(LoadSignalTest, recentlyActiveWindowsHaveStrictBoundaries) {
  TaskClockGuard clock(0);
  LoadRegistry registry({2, 4});
  LoadRegistry::Registration owner(registry), reader(registry);
  { LoadScope load(owner, LoadKind::REQUEST); }
  for (size_t w = 1; w < LOAD_WINDOWS_NS.size(); w++) {
    taskClockNsForTests.store((int64_t)LOAD_WINDOWS_NS[w] - 1);
    EXPECT_EQ(1u, reader.snapshot()[w]);
    taskClockNsForTests.store((int64_t)LOAD_WINDOWS_NS[w]);
    EXPECT_EQ(0u, reader.snapshot()[w]);
  }
}

TEST(LoadSignalTest, shareScopesLeaveNoRecency) {
  TaskClockGuard clock(0);
  LoadRegistry registry({2, 4});
  LoadRegistry::Registration owner(registry), reader(registry);
  auto reads = taskClockReadsForTests.load();
  {
    LoadScope share(owner, LoadKind::SHARE);
    { LoadScope nestedRequest(owner, LoadKind::REQUEST); }
    EXPECT_EQ(1u, reader.snapshot()[0]);
    reads = taskClockReadsForTests.load();
  }
  EXPECT_EQ(reads, taskClockReadsForTests.load());
  EXPECT_EQ((std::array<uint32_t, 4>{0, 0, 0, 0}), reader.snapshot());
  {
    LoadRegistry::ArenaActivity arena(registry);
    arena.begin(owner);
    reads = taskClockReadsForTests.load();
    arena.end(owner);
    EXPECT_EQ(reads, taskClockReadsForTests.load());
    EXPECT_EQ((std::array<uint32_t, 4>{0, 0, 0, 0}), reader.snapshot());
  }
  { LoadScope request(owner, LoadKind::REQUEST); }
  taskClockNsForTests.store(249'999);
  EXPECT_EQ((std::array<uint32_t, 4>{0, 1, 1, 1}), reader.snapshot());
}

TEST(LoadSignalTest, budgetTiers) {
  TaskClockGuard clock(0);
  LoadRegistry registry({4, 8});
  LoadRegistry::Registration caller(registry);
  std::array<std::unique_ptr<LoadRegistry::Registration>, 7> others;
  FanoutLoadGuard load(&caller);
  for (size_t hot = 0; hot <= others.size(); hot++) {
    if (hot) {
      others[hot - 1] = std::make_unique<LoadRegistry::Registration>(registry);
      ASSERT_TRUE(others[hot - 1]->begin());
    }
    for (uint64_t sibling : {FANOUT_GRAIN_NEVER, uint64_t{1'000'000}, uint64_t{1}}) {
      SCOPED_TRACE(::testing::Message() << "hot=" << hot << ", sibling grain=" << sibling);
      fanoutSiblingGrainNs.store(sibling);
      auto reads = taskClockReadsForTests.load();
      auto budget = LoadRegistry::fanoutBudget(50'000);
      EXPECT_EQ(reads + 1, taskClockReadsForTests.load());
      EXPECT_EQ(LoadRegistry::classify((uint32_t)hot, {4, 8}), budget.cls);
      if (hot < 3) {
        EXPECT_EQ(50'000u, budget.grainNs);
        EXPECT_EQ(3 - hot, budget.maxPublished);
      } else if (hot < 7 && sibling != FANOUT_GRAIN_NEVER) {
        EXPECT_EQ(std::max(uint64_t{50'000}, sibling), budget.grainNs);
        EXPECT_EQ(7 - hot, budget.maxPublished);
      } else {
        EXPECT_EQ(FANOUT_GRAIN_NEVER, budget.grainNs);
        EXPECT_EQ(0u, budget.maxPublished);
      }
    }
  }
  EXPECT_NE(std::string::npos, registry.debugText().find(
      "point=\"fanout_decision\",window_us=\"250\",class=\"idle_core\"} 9\n"));
  EXPECT_NE(std::string::npos, registry.debugText().find(
      "point=\"fanout_decision\",window_us=\"250\",class=\"siblings_only\"} 12\n"));
  EXPECT_NE(std::string::npos, registry.debugText().find(
      "point=\"fanout_decision\",window_us=\"250\",class=\"saturated\"} 3\n"));
}

TEST(LoadSignalTest, classifiesPhysicalAndLogicalBoundaries) {
  constexpr std::array<LoadClass, 5> expected{
      LoadClass::IDLE_CORE, LoadClass::SIBLINGS_ONLY, LoadClass::SIBLINGS_ONLY,
      LoadClass::SATURATED, LoadClass::SATURATED};
  for (uint32_t hot = 0; hot < expected.size(); hot++) {
    EXPECT_EQ(expected[hot], LoadRegistry::classify(hot, {2, 4}));
  }
  EXPECT_EQ(LoadClass::IDLE_CORE, LoadRegistry::classify(0, {2, 2}));
  EXPECT_EQ(LoadClass::SATURATED, LoadRegistry::classify(1, {2, 2}));
  EXPECT_EQ(LoadClass::SATURATED, LoadRegistry::classify(0, {1, 1}));
}

TEST(LoadSignalTest, requestsDoNotSample) {
  TaskClockGuard clock(0);
  LoadRegistry registry({2, 4});
  LoadRegistry::Registration owner(registry);
  FanoutLoadGuard load(&owner);
  auto reads = taskClockReadsForTests.load();
  LoadSlot* cached = nullptr;
  for (int i = 0; i < 128; i++) {
    LoadScope request(LoadKind::REQUEST);
    LoadScope nestedRequest(LoadKind::REQUEST);
    LoadScope share(LoadKind::SHARE);
    ASSERT_NE(nullptr, LoadRegistry::localSlot);
    if (i == 0) cached = LoadRegistry::localSlot;
    EXPECT_EQ(cached, LoadRegistry::localSlot);
  }
  EXPECT_EQ(reads + 128, taskClockReadsForTests.load());  // Only request ends.
  EXPECT_EQ(std::string::npos, registry.debugText().find("request_start"));
  EXPECT_NE(std::string::npos, registry.debugText().find(
      "point=\"fanout_decision\",window_us=\"0\",class=\"idle_core\"} 0\n"));
  owner.sample();
  EXPECT_NE(std::string::npos, registry.debugText().find(
      "point=\"fanout_decision\",window_us=\"0\",class=\"idle_core\"} 1\n"));
}

TEST(LoadSignalTest, recordsAllWindowsAndHotSums) {
  TaskClockGuard clock(100);
  LoadRegistry registry({2, 4});
  LoadRegistry::Registration caller(registry), other(registry);
  LoadScope self(caller, LoadKind::REQUEST);
  { LoadScope work(other, LoadKind::REQUEST); }
  caller.sample();
  for (size_t w = 0; w < LOAD_WINDOWS_NS.size(); w++) {
    std::string labels = "point=\"fanout_decision\",window_us=\"" + std::to_string(LOAD_WINDOWS_NS[w] / 1000) + "\"";
    auto text = registry.debugText();
    EXPECT_NE(std::string::npos, text.find("luxir_debug_load_samples_total{" + labels
        + ",class=\"" + (w ? "siblings_only" : "idle_core") + "\"} 1\n"));
    EXPECT_NE(std::string::npos, text.find("luxir_debug_load_hot_threads_sum{" + labels + "} " + (w ? "1\n" : "0\n")));
  }
}

TEST(LoadSignalTest, arenaDetachDoesNotStrandTaskNesting) {
  TaskClockGuard clock(100);
  LoadRegistry registry({2, 4});
  LoadRegistry::Registration owner(registry), reader(registry);
  {
    LoadRegistry::ArenaActivity arena(registry), otherArena(registry);
    arena.begin(owner);
    otherArena.begin(owner);
    { LoadScope task(owner, LoadKind::SHARE); }
    EXPECT_EQ(1u, reader.snapshot()[0]);
    EXPECT_EQ(0u, owner.snapshot()[0]);
    arena.end(owner);
    EXPECT_EQ(1u, reader.snapshot()[0]);
    // Detach the other arena without a matching exit, as TBB may do.
  }
  EXPECT_EQ(0u, reader.snapshot()[0]);
  auto reads = taskClockReadsForTests.load();
  { LoadScope task(owner, LoadKind::SHARE); }
  EXPECT_EQ(reads, taskClockReadsForTests.load());
}

TEST(LoadSignalTest, exhaustedSlotsCanBeReusedWithoutArenaHistory) {
  TaskClockGuard clock(100);
  LoadRegistry registry({2, 4});
  LoadRegistry::Registration reader(registry);
  LoadRegistry::ArenaActivity arena(registry);
  std::vector<std::unique_ptr<LoadRegistry::Registration>> owners;
  for (int i = 0; i < 256; i++) {
    auto owner = std::make_unique<LoadRegistry::Registration>(registry);
    ASSERT_TRUE(owner->begin());
    owner->end(false);
    owners.push_back(std::move(owner));
  }
  arena.begin(*owners.back());
  EXPECT_EQ(1u, reader.snapshot()[0]);
  EXPECT_FALSE(reader.begin());
  EXPECT_NE(std::string::npos, registry.debugText().find("luxir_debug_load_slots_exhausted_total 1\n"));
  owners.pop_back();
  EXPECT_TRUE(reader.begin());
  reader.end(false);
  // All remaining registrations are idle, and the reused slot has no arena bit.
  LoadRegistry::Registration observer(registry);
  EXPECT_EQ(0u, observer.snapshot()[0]);
}

TEST(LoadSignalTest, threadExitReleasesTlsRegistration) {
  TaskClockGuard clock(100);
  LoadRegistry registry({2, 4});
  LoadRegistry::Registration reader(registry);
  for (int i = 0; i < 257; i++) {
    std::thread worker([&] {
      thread_local LoadRegistry::Registration owner(registry);
      FanoutLoadGuard load(&owner);
      { LoadScope request(LoadKind::REQUEST); }
      EXPECT_TRUE(owner.begin());  // Even an unfinished scope ends with the thread.
      EXPECT_EQ(1u, reader.snapshot()[0]);
    });
    worker.join();
  }
  EXPECT_EQ((std::array<uint32_t, 4>{0, 0, 0, 0}), reader.snapshot());
  EXPECT_NE(std::string::npos, registry.debugText().find("luxir_debug_load_slots_exhausted_total 0\n"));
}

TEST(LoadSignalTest, indexObserverTracksWorkersAndDetaches) {
  bool saved = LoadRegistry::enabled.exchange(true);
  auto restore = scope_guard([&] { LoadRegistry::enabled.store(saved); });
  auto before = LoadRegistry::local().snapshot()[0];
  std::latch started(1), resume(1), done(1);
  {
    LuxirConfig config;
    config.store.backend = "ram";
    config.server.workers = 1;
    LuxirNode node(config);
    node.getIndexArena().enqueue([&] {
      started.count_down();
      resume.wait();
      done.count_down();
    });
    started.wait();
    EXPECT_EQ(before + 1, LoadRegistry::local().snapshot()[0]);
    resume.count_down();
    done.wait();
  }
  EXPECT_EQ(before, LoadRegistry::local().snapshot()[0]);
}

TEST(LoadSignalTest, disabledScopesSamplingAndTextDoNotReadClock) {
  bool saved = LoadRegistry::enabled.exchange(false);
  auto restore = scope_guard([&] { LoadRegistry::enabled.store(saved); });
  TaskClockGuard clock(0);
  auto reads = taskClockReadsForTests.load();
  { LoadScope request(LoadKind::REQUEST); LoadScope share(LoadKind::SHARE); }
  LoadRegistry::sample();
  EXPECT_EQ(std::string::npos, loadSignalDebugText().find("luxir_debug_load_"));
  EXPECT_EQ(reads, taskClockReadsForTests.load());
}

TEST(LoadSignalTest, optionsAreApplied) {
  bool saved = LoadRegistry::enabled.load();
  auto grain = fanoutGrainNs.load();
  auto sibling = fanoutSiblingGrainNs.load();
  auto level = spdlog::get_level();
  auto restore = scope_guard([&] {
    LoadRegistry::enabled.store(saved);
    fanoutGrainNs.store(grain);
    fanoutSiblingGrainNs.store(sibling);
    spdlog::set_level(level);
  });
  LuxirConfig config;
  config.apply();
  EXPECT_TRUE(LoadRegistry::enabled.load());
  CLI::App app;
  config.addOptions(app);
  app.parse("--search.fanout-grain-us=-1");
  config.apply();
  EXPECT_FALSE(LoadRegistry::enabled.load());
  auto text = loadSignalDebugText();
  EXPECT_NE(std::string::npos, text.find("luxir_debug_fanout_share_credit_ns_sum "
      + std::to_string(fanoutShareCreditNsSum.load()) + "\n"));
  EXPECT_NE(std::string::npos, text.find("luxir_debug_fanout_share_decisions_total{shares=\"2\"} "
      + std::to_string(fanoutShareDecisions[0].load()) + "\n"));
}


}
