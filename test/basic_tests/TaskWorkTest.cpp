// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <CLI/CLI.hpp>
#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>

#include <array>
#include <memory>
#include <thread>
#include <vector>
#include <oneapi/tbb/task_arena.h>

#include "luxir/LuxirConfig.h"
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TaskSpawnCounter.h"
#include "test/TestUtils.h"

namespace luxir::test {

class TaskWorkTest : public ::testing::Test {
  FanoutLoadGuard load;
};

TEST_F(TaskWorkTest, sharesBalanceUnequalChildrenAndExtremeEstimates) {
  std::array<TaskWork, 5> costs{55'000, 5'000, 40'000, 5'000, 55'000};
  auto shares = planTaskShares(costs, 50'000, costs.size());
  ASSERT_EQ(3u, shares.size());
  size_t next = 0;
  uint64_t smallest = UINT64_MAX, largest = 0;
  for (auto share : shares) {
    EXPECT_EQ(next, share.begin);
    EXPECT_GT(share.end, share.begin);
    EXPECT_GE(share.work, 50'000u);
    smallest = std::min(smallest, share.work);
    largest = std::max(largest, share.work);
    next = share.end;
  }
  EXPECT_EQ(costs.size(), next);
  EXPECT_LE(largest, smallest * 2);
  EXPECT_EQ(2u, planTaskShares(costs, 50'000, 2).size());
  EXPECT_TRUE(planTaskShares(costs, 50'000, 1).empty());
  EXPECT_TRUE(planTaskShares(costs, 50'000, 0).empty());
  EXPECT_TRUE(planTaskShares(std::array<TaskWork, 2>{49'999, 50'000}, 50'000, 4).empty());
  EXPECT_TRUE(planTaskShares(std::array<TaskWork, 4>{150'000, 1, 1, 1}, 50'000, 4).empty());
  EXPECT_TRUE(planTaskShares(std::array<TaskWork, 2>{1'000'000, 50'000}, 50'000, 4).empty());
  EXPECT_EQ(2u, planTaskShares(std::array<TaskWork, 2>{UINT64_MAX, UINT64_MAX}, UINT64_MAX, 4).size());
  EXPECT_EQ(UINT64_MAX, estimateTaskWork(UINT64_MAX, 2));
  EXPECT_EQ(UINT64_MAX, taskWorkAdd(UINT64_MAX - 1, 2));
}

TEST_F(TaskWorkTest, stealIsChargedOncePerShare) {
  std::array<TaskWork, 8> costs;
  costs.fill({15'000, 9'000});
  auto shares = planTaskShares(costs, 50'000, costs.size());
  ASSERT_EQ(2u, shares.size());
  EXPECT_EQ(9'000u, shares[0].steal);
  EXPECT_EQ(60'000u, shares[0].work);
  costs.fill({15'000, 20'000});
  EXPECT_TRUE(planTaskShares(costs, 50'000, costs.size()).empty());
  // Only the last share misses its steal floor; it must merge too.
  EXPECT_TRUE(planTaskShares(std::array<TaskWork, 2>{{{60'000, 0}, {60'000, 20'000}}},
                            50'000, 2).empty());
}

TEST_F(TaskWorkTest, everyPolicyPublishesEverySibling) {
  FanoutGrainGuard grain(FANOUT_GRAIN_NEVER);
  TaskClockGuard clock(0);
  auto reads = taskClockReadsForTests.load();
  auto caller = std::this_thread::get_id();
  std::vector<size_t> made;
  std::array<int, 4> completed{};
  FanoutScope runner(nullptr, FanoutPolicy::EVERY);
  TaskSpawnCounter counter;
  EXPECT_EQ(3u, task_group_fanout(&runner, completed.size(), FanoutKind::EXHAUSTIVE, [](size_t) {
    ADD_FAILURE() << "EVERY asked for sizes";
    return FanoutSize{};
  }, [&](size_t i) {
    EXPECT_EQ(caller, std::this_thread::get_id());
    made.push_back(i);
    return [&, i] {
      if (i == 3) { EXPECT_EQ(caller, std::this_thread::get_id()); }
      completed[i]++;
    };
  }));
  runner.join();
  EXPECT_EQ(3, counter.count());
  EXPECT_EQ((std::vector<size_t>{0, 1, 2, 3}), made);
  EXPECT_EQ((std::array<int, 4>{1, 1, 1, 1}), completed);
  EXPECT_EQ(reads + 6, taskClockReadsForTests.load());  // Publish and start per share.
}

TEST_F(TaskWorkTest, publishedShareRecordsDelayAndRunTime) {
  FanoutGrainGuard grain(50'000);
  FanoutLoadGuard load;
  TaskClockGuard clock(100);
  oneapi::tbb::task_arena arena(1);  // No share starts before join.
  for (size_t children : {1u, 2u}) arena.execute([&] {
    taskClockNsForTests.store(100);
    auto delay = fanoutShareStartDelayNsSum.load();
    auto count = fanoutShareStartDelayCount.load();
    auto maximum = fanoutShareStartDelayNsMax.load();
    auto run = fanoutShareRunNsSum.load();
    auto estimate = fanoutShareEstimatedNsSum.load();
    auto publisherRuns = fanoutSharePublisherRuns.load();
    FanoutScope runner(nullptr, FanoutPolicy::EVERY);
    auto makeTask = [](size_t) { return [] { taskClockNsForTests.fetch_add(40); }; };
    detail::publishFanoutShare(&runner, 0, children, 120, makeTask);
    taskClockNsForTests.store(130);
    runner.join();
    EXPECT_EQ(delay + 30, fanoutShareStartDelayNsSum.load());
    EXPECT_EQ(count + 1, fanoutShareStartDelayCount.load());
    EXPECT_EQ(std::max(maximum, uint64_t{30}), fanoutShareStartDelayNsMax.load());
    EXPECT_EQ(run + children * 40, fanoutShareRunNsSum.load());
    EXPECT_EQ(estimate + 120, fanoutShareEstimatedNsSum.load());
    EXPECT_EQ(publisherRuns + 1, fanoutSharePublisherRuns.load());
    auto text = loadSignalDebugText();
    EXPECT_NE(std::string::npos, text.find("luxir_debug_fanout_share_start_delay_ns_sum "
        + std::to_string(delay + 30) + "\n"));
    EXPECT_NE(std::string::npos, text.find("luxir_debug_fanout_share_run_ns_sum "
        + std::to_string(run + children * 40) + "\n"));
  });
}

TEST_F(TaskWorkTest, neverRunsForwardWithoutClockOrSizes) {
  FanoutGrainGuard grain(FANOUT_GRAIN_NEVER);
  TaskClockGuard clock(0);
  auto reads = taskClockReadsForTests.load();
  std::vector<size_t> order;
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(0u, task_group_fanout(&runner, 5, FanoutKind::EXHAUSTIVE, [](size_t) {
    ADD_FAILURE() << "never asked for sizes";
    return FanoutSize{};
  }, [&](size_t i) { return [&, i] { order.push_back(i); }; }));
  runner.join();
  EXPECT_EQ((std::vector<size_t>{0, 1, 2, 3, 4}), order);
  EXPECT_EQ(0, counter.count());
  EXPECT_EQ(reads, taskClockReadsForTests.load());
}

TEST_F(TaskWorkTest, nullEmptyAndUnaryDoNotReadClockOrSizes) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  auto reads = taskClockReadsForTests.load();
  std::vector<size_t> order;
  auto sizeOf = [](size_t) {
    ADD_FAILURE() << "serial fast path asked for sizes";
    return FanoutSize{};
  };
  auto makeTask = [&](size_t i) { return [&, i] { order.push_back(i); }; };
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(0u, task_group_fanout(nullptr, 3, FanoutKind::EXHAUSTIVE, sizeOf, makeTask));
  EXPECT_EQ((std::vector<size_t>{0, 1, 2}), order);
  order.clear();
  EXPECT_EQ(0u, task_group_fanout(&runner, 0, FanoutKind::EXHAUSTIVE, sizeOf, makeTask));
  EXPECT_TRUE(order.empty());
  EXPECT_EQ(0u, task_group_fanout(&runner, 1, FanoutKind::EXHAUSTIVE, sizeOf, makeTask));
  EXPECT_EQ((std::vector<size_t>{0}), order);
  runner.join();
  EXPECT_EQ(0, counter.count());
  EXPECT_EQ(reads, taskClockReadsForTests.load());
}

TEST_F(TaskWorkTest, noCreditRunsBackwardWithoutSizing) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  std::vector<size_t> order;
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(0u, task_group_fanout(&runner, 5, FanoutKind::EXHAUSTIVE, [](size_t) {
    ADD_FAILURE() << "sized before earning credit";
    return FanoutSize{};
  }, [&](size_t i) { return [&, i] { order.push_back(i); }; }));
  runner.join();
  EXPECT_EQ((std::vector<size_t>{4, 3, 2, 1, 0}), order);
  EXPECT_EQ(0, counter.count());
}

TEST_F(TaskWorkTest, twoChildrenStaySerial) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  auto reads = taskClockReadsForTests.load();
  std::vector<size_t> order;
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(0u, task_group_fanout(&runner, 2, FanoutKind::EXHAUSTIVE, [](size_t) {
    ADD_FAILURE() << "two children asked for sizes";
    return FanoutSize{};
  }, [&](size_t i) { return [&, i] {
    order.push_back(i);
    taskClockNsForTests.fetch_add(1'000'000);
  }; }));
  runner.join();
  EXPECT_EQ((std::vector<size_t>{1, 0}), order);
  EXPECT_EQ(0, counter.count());
  EXPECT_EQ(reads, taskClockReadsForTests.load());
}

TEST_F(TaskWorkTest, preCreditChecksAreGeometric) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  for (size_t n : {5u, 45u}) {
    FanoutCreditScope credit(0);
    FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
    auto reads = taskClockReadsForTests.load();
    auto before = reads;
    std::vector<size_t> checks;
    EXPECT_EQ(0u, task_group_fanout(&runner, n, FanoutKind::EXHAUSTIVE, [](size_t) {
      ADD_FAILURE() << "sized before earning credit";
      return FanoutSize{};
    }, [&](size_t i) { return [&, i] {
      if (taskClockReadsForTests.load() != reads) {
        checks.push_back(n - i - 1);
        reads = taskClockReadsForTests.load();
      }
    }; }));
    runner.join();
    // The first read starts the rate interval, before any child has run; the
    // last is the final point where two children remain.
    EXPECT_EQ((n == 5 ? std::vector<size_t>{0, 1, 2, 3}
                      : std::vector<size_t>{0, 1, 2, 4, 8, 16, 32, 43}), checks);
    EXPECT_EQ(before + (n == 5 ? 4 : 8), taskClockReadsForTests.load());
    EXPECT_FALSE(runner.published());
  }
}

TEST_F(TaskWorkTest, earnedCreditChecksEveryChild) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  FanoutCreditScope credit(0);
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  size_t ran = 0;
  auto reads = taskClockReadsForTests.load();
  auto before = reads;
  std::vector<size_t> checks;
  EXPECT_EQ(0u, task_group_fanout(&runner, 16, FanoutKind::EXHAUSTIVE, [](size_t) {
    return FanoutSize{1, UINT64_MAX};  // Earn credit but never afford a share.
  }, [&](size_t) { return [&] {
    if (taskClockReadsForTests.load() != reads) {
      checks.push_back(ran);
      reads = taskClockReadsForTests.load();
    }
    if (++ran == 3) taskClockNsForTests.store(50'000);
  }; }));
  runner.join();
  EXPECT_EQ(16u, ran);
  EXPECT_EQ((std::vector<size_t>{0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}), checks);
  EXPECT_EQ(before + 14, taskClockReadsForTests.load());
  EXPECT_FALSE(runner.published());
}

TEST_F(TaskWorkTest, creditAndEvidencePublishBalancedShares) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  auto caller = std::this_thread::get_id();
  std::array<int, 8> completed{};
  std::array<std::vector<size_t>, 4> order;
  std::vector<size_t> made;
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  size_t borrowed = 0;
  EXPECT_EQ(2u, task_group_fanout(&runner, completed.size(), FanoutKind::EXHAUSTIVE, [&](size_t) {
    EXPECT_EQ((std::vector<size_t>{7, 6}), made);
    return FanoutSize{1, 0};
  }, [&](size_t i) {
    EXPECT_EQ(caller, std::this_thread::get_id());
    made.push_back(i);
    borrowed = i;
    return [&, owned = std::make_unique<size_t>(borrowed)] {
      size_t i = *owned;
      completed[i]++;
      order[i / 2].push_back(i);
      if (std::this_thread::get_id() == caller) taskClockNsForTests.fetch_add(30'000);
      if (i >= 4) { EXPECT_EQ(caller, std::this_thread::get_id()); }
    };
  }));
  borrowed = 100;
  runner.join();
  EXPECT_EQ(2, counter.count());
  EXPECT_EQ((std::vector<size_t>{7, 6, 0, 1, 2, 3, 5, 4}), made);
  for (size_t i = 0; i < order.size(); i++) {
    EXPECT_EQ((std::vector<size_t>{i * 2 + 1, i * 2}), order[i]);
  }
  EXPECT_EQ((std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1}), completed);
}

TEST_F(TaskWorkTest, sizesScaleTheMeasuredRate) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  auto caller = std::this_thread::get_id();
  std::array<int, 8> completed{};
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(0u, task_group_fanout(&runner, completed.size(), FanoutKind::EXHAUSTIVE, [](size_t i) {
    return FanoutSize{i == 7 ? 1000u : 1u, 0};
  }, [&](size_t i) { return [&, i] {
    completed[i]++;
    if (i == 7 && std::this_thread::get_id() == caller) taskClockNsForTests.fetch_add(100'000);
  }; }));
  runner.join();
  EXPECT_EQ(0, counter.count());
  EXPECT_EQ((std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1}), completed);
}

TEST_F(TaskWorkTest, latestIntervalSetsTheRemainingRate) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  auto caller = std::this_thread::get_id();
  std::array<int, 8> completed{};
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(2u, task_group_fanout(&runner, completed.size(), FanoutKind::EXHAUSTIVE, [](size_t i) {
    return FanoutSize{i == 7 ? 1000u : 1u, 0};
  }, [&](size_t i) { return [&, i] {
    completed[i]++;
    if (std::this_thread::get_id() == caller) {
      // The first sample prices tiny remaining children below the grain.
      // The second sample supports shares; a cumulative rate still would not.
      taskClockNsForTests.fetch_add(i == 7 ? 100'000 : 30'000);
    }
  }; }));
  runner.join();
  EXPECT_EQ(2, counter.count());
  EXPECT_EQ((std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1}), completed);
}

TEST_F(TaskWorkTest, exhaustiveCallerKeepsTheLargestShare) {
  FanoutGrainGuard grain(10'000);
  FanoutLoadGuard load;
  auto caller = std::this_thread::get_id();
  // Also keep a middle share, publishing children on both sides of it.
  for (bool middle : {false, true}) {
    TaskClockGuard clock(0);
    auto credit = fanoutShareCreditNsSum.load();
    auto decisions = fanoutShareCreditCount.load();
    auto threeShares = fanoutShareDecisions[1].load();
    auto kept = fanoutShareKeptEstimatedNsSum.load();
    auto largestPublished = fanoutShareLargestPublishedEstimatedNsSum.load();
    std::array<uint64_t, 5> sizes{40, 30, 15, 10, 5};
    if (middle) std::swap(sizes[0], sizes[1]);
    std::array<int, 5> completed{};
    std::vector<size_t> inlineOrder, made;
    FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
    EXPECT_EQ(2u, task_group_fanout(&runner, sizes.size(), FanoutKind::EXHAUSTIVE,
        [&](size_t i) { return FanoutSize{sizes[i], 0}; }, [&](size_t i) {
      made.push_back(i);
      return [&, i] {
        completed[i]++;
        if (std::this_thread::get_id() == caller) {
          inlineOrder.push_back(i);
          taskClockNsForTests.fetch_add((int64_t)sizes[i] * 1000);
        }
      };
    }));
    // Inspect before join: the caller may execute queued tasks while waiting.
    EXPECT_EQ((std::vector<size_t>{4, 3, middle ? 1u : 0u}), inlineOrder);
    EXPECT_EQ((std::vector<size_t>{4, 3, middle ? 0u : 1u, 2, middle ? 1u : 0u}), made);
    runner.join();
    EXPECT_EQ((std::array<int, 5>{1, 1, 1, 1, 1}), completed);
    EXPECT_EQ(credit + 15'000, fanoutShareCreditNsSum.load());
    EXPECT_EQ(decisions + 1, fanoutShareCreditCount.load());
    EXPECT_EQ(threeShares + 1, fanoutShareDecisions[1].load());
    EXPECT_EQ(kept + 40'000, fanoutShareKeptEstimatedNsSum.load());
    EXPECT_EQ(largestPublished + 30'000, fanoutShareLargestPublishedEstimatedNsSum.load());
  }
}

TEST_F(TaskWorkTest, prunedFirstChildIsNotEvidence) {
  FanoutGrainGuard grain(50'000);
  FanoutLoadGuard load;
  auto caller = std::this_thread::get_id();
  for (int64_t later : {1'000, 100'000}) {
    TaskClockGuard clock(0);
    std::array<int, 8> completed{};
    std::vector<size_t> inlineOrder;
    FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
    EXPECT_EQ(later == 1'000 ? 0u : 5u,
        task_group_fanout(&runner, completed.size(), FanoutKind::PRUNED,
            [](size_t) { return FanoutSize{1, 0}; }, [&](size_t i) { return [&, i] {
      completed[i]++;
      if (std::this_thread::get_id() == caller) {
        inlineOrder.push_back(i);
        taskClockNsForTests.fetch_add(i == 7 ? 1'000'000 : later);
      }
    }; }));
    EXPECT_EQ((later == 1'000 ? std::vector<size_t>{7, 6, 5, 4, 3, 2, 1, 0}
                             : std::vector<size_t>{7, 6, 5}), inlineOrder);
    runner.join();
    EXPECT_EQ((std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1}), completed);
  }
}

TEST_F(TaskWorkTest, serialKindNeverPublishesUnderAGrain) {
  for (auto policy : {FanoutPolicy::ADAPTIVE, FanoutPolicy::EVERY}) {
    bool adaptive = policy == FanoutPolicy::ADAPTIVE;
    FanoutGrainGuard grain(50'000);
    TaskClockGuard clock(0);
    auto reads = taskClockReadsForTests.load();
    std::array<int, 5> completed{};
    std::vector<size_t> made;
    FanoutScope runner(nullptr, policy);
    EXPECT_EQ(adaptive ? 0u : 4u, task_group_fanout(&runner, completed.size(), FanoutKind::SERIAL,
        [](size_t) {
      ADD_FAILURE() << "serial kind asked for sizes";
      return FanoutSize{};
    }, [&](size_t i) {
      made.push_back(i);
      return [&, i] { completed[i]++; };
    }));
    runner.join();
    EXPECT_EQ((adaptive ? std::vector<size_t>{4, 3, 2, 1, 0} : std::vector<size_t>{0, 1, 2, 3, 4}), made);
    EXPECT_EQ((std::array<int, 5>{1, 1, 1, 1, 1}), completed);
    EXPECT_EQ(reads + (adaptive ? 0 : 12), taskClockReadsForTests.load());
  }
}

TEST_F(TaskWorkTest, loadDenialResetsCredit) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  // Exercise both denied classes, including saturation with no SMT.
  for (LoadTopology topology : {LoadTopology{1, 1}, LoadTopology{1, 2}}) {
    taskClockNsForTests.store(0);
    LoadRegistry registry(topology);
    LoadRegistry::Registration caller(registry);
    FanoutLoadGuard load(&caller);
    FanoutCreditScope credit;
    size_t denial = topology.logical == 1 ? 1 : 0;
    auto before = fanoutLoadDenials[denial].load();
    std::vector<size_t> order;
    FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
    EXPECT_EQ(0u, task_group_fanout(&runner, 10, FanoutKind::EXHAUSTIVE,
        [](size_t) { return FanoutSize{1, 0}; }, [&](size_t i) { return [&, i] {
      order.push_back(i);
      taskClockNsForTests.fetch_add(25'000);
    }; }));
    runner.join();
    EXPECT_EQ((std::vector<size_t>{9, 8, 7, 6, 5, 4, 3, 2, 1, 0}), order);
    EXPECT_EQ(150'000u, fanoutTaskStart);
    EXPECT_EQ(before + 3, fanoutLoadDenials[denial].load());
    EXPECT_NE(std::string::npos, registry.debugText().find(
        "point=\"fanout_decision\",window_us=\"250\",class=\""
        + std::string(denial ? "saturated" : "siblings_only") + "\"} 3\n"));
  }
}

TEST_F(TaskWorkTest, shareCapFollowsIdlePhysicalCores) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  LoadRegistry registry({4, 8});
  LoadRegistry::Registration caller(registry), other(registry);
  LoadScope busy(other, LoadKind::SHARE);
  FanoutLoadGuard load(&caller);
  auto callerThread = std::this_thread::get_id();
  std::array<int, 12> completed{};
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(2u, task_group_fanout(&runner, completed.size(), FanoutKind::EXHAUSTIVE,
      [](size_t) { return FanoutSize{1, 0}; }, [&](size_t i) { return [&, i] {
    completed[i]++;
    if (i == 11 && std::this_thread::get_id() == callerThread) taskClockNsForTests.fetch_add(50'000);
  }; }));
  runner.join();
  EXPECT_EQ(2, counter.count());
  for (int count : completed) EXPECT_EQ(1, count);
}

TEST_F(TaskWorkTest, siblingGrainCanRejectAfterTheSnapshot) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  LoadRegistry registry({1, 2});
  LoadRegistry::Registration caller(registry);
  FanoutLoadGuard load(&caller, 1'000'000);
  FanoutCreditScope credit;
  auto denials = fanoutLoadDenials[0].load();
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  EXPECT_EQ(0u, task_group_fanout(&runner, 8, FanoutKind::EXHAUSTIVE,
      [](size_t) { return FanoutSize{1, 0}; }, [](size_t) { return [] {
    taskClockNsForTests.fetch_add(50'000);
  }; }));
  runner.join();
  EXPECT_EQ(300'000u, fanoutTaskStart);
  EXPECT_EQ(denials, fanoutLoadDenials[0].load());
  EXPECT_NE(std::string::npos, registry.debugText().find(
      "point=\"fanout_decision\",window_us=\"250\",class=\"siblings_only\"} 6\n"));
}

TEST_F(TaskWorkTest, stealBlocksSharesThatCannotPayIt) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  auto caller = std::this_thread::get_id();
  std::array<int, 8> completed{};
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(0u, task_group_fanout(&runner, completed.size(), FanoutKind::EXHAUSTIVE, [](size_t) {
    return FanoutSize{1, 4};
  }, [&](size_t i) { return [&, i] {
    completed[i]++;
    if (std::this_thread::get_id() == caller) taskClockNsForTests.fetch_add(30'000);
  }; }));
  runner.join();
  EXPECT_EQ(0, counter.count());
  EXPECT_EQ((std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1}), completed);
}

TEST_F(TaskWorkTest, publishResetsCredit) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(0);
  FanoutCreditScope credit;
  auto caller = std::this_thread::get_id();
  FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
  TaskSpawnCounter counter;
  EXPECT_EQ(2u, task_group_fanout(&runner, 8, FanoutKind::EXHAUSTIVE, [](size_t) { return FanoutSize{1, 0}; },
      [&](size_t i) { return [&, i] {
    if (i >= 6 && std::this_thread::get_id() == caller) taskClockNsForTests.fetch_add(30'000);
  }; }));
  EXPECT_EQ(60'000u, fanoutTaskStart);
  EXPECT_EQ(60'000, taskClockNsForTests.load());
  EXPECT_EQ(0u, task_group_fanout(&runner, 4, FanoutKind::EXHAUSTIVE, [](size_t) {
    ADD_FAILURE() << "reused credit after publishing";
    return FanoutSize{1, 0};
  }, [](size_t) { return [] {}; }));
  runner.join();
  EXPECT_EQ(2, counter.count());
}

TEST_F(TaskWorkTest, creditScopeNestsAndRestores) {
  FanoutGrainGuard grain(50'000);
  TaskClockGuard clock(10);
  auto previous = fanoutTaskStart;
  {
    FanoutCreditScope outer;
    EXPECT_EQ(10u, fanoutTaskStart);
    taskClockNsForTests.store(20);
    {
      FanoutCreditScope inner;
      EXPECT_EQ(20u, fanoutTaskStart);
    }
    EXPECT_EQ(10u, fanoutTaskStart);
    {
      FanoutGrainGuard guard(FANOUT_GRAIN_NEVER);
      FanoutCreditScope inner;
      EXPECT_FALSE(fanoutTaskStart);
    }
    EXPECT_EQ(10u, fanoutTaskStart);
  }
  EXPECT_EQ(previous, fanoutTaskStart);
}

TEST_F(TaskWorkTest, creditScopeTakesAGivenStart) {
  TaskClockGuard clock(100);
  auto previous = fanoutTaskStart;
  auto reads = taskClockReadsForTests.load();
  for (uint64_t value : {uint64_t{50'000}, FANOUT_GRAIN_NEVER}) {
    FanoutGrainGuard grain(value);
    for (uint64_t start : {uint64_t{0}, uint64_t{10}}) {
      {
        FanoutCreditScope credit(start);
        if (value == 50'000) { EXPECT_EQ(start, fanoutTaskStart); }
        else { EXPECT_FALSE(fanoutTaskStart); }
        EXPECT_EQ(reads, taskClockReadsForTests.load());
      }
      EXPECT_EQ(previous, fanoutTaskStart);
    }
  }
}

TEST_F(TaskWorkTest, unsizedFormIsSerialUnderAGrain) {
  for (uint64_t value : {uint64_t{50'000}, FANOUT_GRAIN_NEVER}) {
    FanoutGrainGuard grain(value);
    TaskClockGuard clock(0);
    auto reads = taskClockReadsForTests.load();
    std::vector<size_t> order;
    FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
    TaskSpawnCounter counter;
    EXPECT_EQ(0u, task_group_fanout(&runner, 4, [&](size_t i) {
      return [&, i] { order.push_back(i); };
    }));
    runner.join();
    EXPECT_EQ((std::vector<size_t>{0, 1, 2, 3}), order);
    EXPECT_EQ(0, counter.count());
    EXPECT_EQ(reads, taskClockReadsForTests.load());
  }
}

TEST_F(TaskWorkTest, groupedTasksAndCallerStopOnCancellation) {
  FanoutGrainGuard grain(50'000);
  // Keep published tasks queued until join, so each cancellation point is
  // deterministic without coordinating worker threads.
  oneapi::tbb::task_arena arena(1);
  for (bool cancelOnCaller : {false, true}) arena.execute([&] {
    TaskClockGuard clock(0);
    FanoutScope runner(nullptr, FanoutPolicy::ADAPTIVE);
    auto* tg = &runner;
    std::array<int, 8> completed{};
    EXPECT_EQ(2u, task_group_fanout(tg, completed.size(), FanoutKind::EXHAUSTIVE, [](size_t) { return FanoutSize{1, 0}; },
        [&](size_t i) { return [&, i] {
      completed[i]++;
      if (i >= 6) taskClockNsForTests.fetch_add(30'000);
      if (i == (cancelOnCaller ? 5u : 3u)) tg->cancel();
    }; }));
    runner.join();
    EXPECT_EQ((cancelOnCaller ? std::array<int, 8>{0, 0, 0, 0, 0, 1, 1, 1}
                             : std::array<int, 8>{0, 0, 0, 1, 1, 1, 1, 1}), completed);
  });
}

TEST_F(TaskWorkTest, cancellationUsesTheSuppliedGroupContext) {
  for (auto policy : {FanoutPolicy::EVERY, FanoutPolicy::ADAPTIVE}) {
    FanoutGrainGuard grain(50'000);
    TaskClockGuard clock(0);
    oneapi::tbb::task_group_context outerContext(oneapi::tbb::task_group_context::isolated);
    oneapi::tbb::task_group outer(outerContext);
    std::array<int, 4> completed{};
    auto caller = std::this_thread::get_id();
    EXPECT_EQ(oneapi::tbb::canceled, outer.run_and_wait([&] {
      outerContext.cancel_group_execution();
      FanoutScope inner(nullptr, policy, true);
      task_group_fanout(&inner, completed.size(), FanoutKind::EXHAUSTIVE, [](size_t) { return FanoutSize{1, 0}; },
          [&](size_t i) { return [&, i] {
        completed[i]++;
        if (std::this_thread::get_id() == caller) taskClockNsForTests.fetch_add(100'000);
      }; });
      inner.join();
    }));
    EXPECT_EQ((std::array<int, 4>{1, 1, 1, 1}), completed);
  }
}

TEST_F(TaskWorkTest, optionsAreApplied) {
  FanoutGrainGuard grain(50'000);
  FanoutLoadGuard load;
  auto savedLevel = spdlog::get_level();
  auto restore = scope_guard([&] {
    spdlog::set_level(savedLevel);
  });
  LuxirConfig defaults;
  defaults.apply();
  EXPECT_EQ(50'000u, fanoutGrainNs.load());
  EXPECT_EQ(1'000'000u, fanoutSiblingGrainNs.load());
  EXPECT_TRUE(LoadRegistry::enabled.load());
  for (int64_t us : {-1, 1, 50, 60'000'000}) {
    LuxirConfig config;
    CLI::App app;
    config.addOptions(app);
    app.parse("--search.fanout-grain-us=" + std::to_string(us)
        + " --search.fanout-sibling-grain-us=" + std::to_string(us));
    config.apply();
    EXPECT_EQ(us < 0 ? FANOUT_GRAIN_NEVER : (uint64_t)us * 1000, fanoutGrainNs.load());
    EXPECT_EQ(us < 0 ? FANOUT_GRAIN_NEVER : (uint64_t)us * 1000, fanoutSiblingGrainNs.load());
  }
  for (const char* option : {"--search.fanout-grain-us=-2", "--search.fanout-grain-us=0", "--search.fanout-grain-us=60000001",
                           "--search.fanout-sibling-grain-us=-2", "--search.fanout-sibling-grain-us=0", "--search.fanout-sibling-grain-us=60000001"}) {
    LuxirConfig config;
    CLI::App app;
    config.addOptions(app);
    EXPECT_THROW(app.parse(option), CLI::ValidationError);
  }
}

TEST_F(TaskWorkTest, adaptiveZeroPublishesNothingBelowTheGrain) {
  FanoutGrainGuard grain(10'000'000'000);
  CollectionHelper helper("fanout_below_grain");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  for (int seg = 0; seg < 5; seg++) {
    helper.index(flatdoc("id", std::to_string(seg), "cat_s", "a"), UpdateMessage::COMMIT);
  }
  ASSERT_EQ(5u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  auto req = localReq(helper.getSearchEngine());
  req->collection("fanout_below_grain").facet("cats", "cat_s");
  TaskSpawnCounter counter;
  req->execute((int32_t)0);
  ASSERT_OK(req);
  EXPECT_EQ(0, counter.count());
}

TEST_F(TaskWorkTest, adaptiveZeroMatchesSerial) {
  CollectionHelper helper("fanout_gate");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  for (int seg = 0; seg < 5; seg++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 64; i++) {
      int64_t value = seg * 64 + i;
      docs.push_back(flatdoc("id", std::to_string(value), "body_w", "alpha beta",
          "cat_s", "c" + std::to_string(i % 8), "sel_s", i % 2 ? "t" : "f", "value_i", value));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
  ASSERT_EQ(5u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  FanoutLoadGuard load;
  auto run = [&](int parallel) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("fanout_gate");
    auto& filtered = req->topDocs("filtered").allQuery().limit(0).matchFilter("sel_s", "t");
    filtered.facet("cats", "cat_s").limit(-1);
    filtered.sum("total", "value_i");
    req->facet("all_cats", "cat_s").limit(-1);
    req->topDocs("ranked").matchQuery("body_w", "alpha").limit(7).fields({"id"});
    auto& sorted = req->topDocs("sorted").allQuery().limit(7).fields({"id", "value_i"});
    qb::sort(sorted, "value_i", qb::DESC);
    auto& exprSort = req->topDocs("expr_sorted").allQuery().limit(7).fields({"id"});
    qb::sort(exprSort, "value_i+1", qb::DESC);
    // Percentile states fold in segment order, including when every inline
    // child arrives back-to-front and the driver must park the states.
    req->expr("median", "percentile(value_i,50)");
    req->rangeFacet("ranges", "value_i").range(0, 320, 64);
    helper.getSearchEngine().dispatch(*req.get(), parallel, SearchEngine::Receiver::MAY_EXECUTE);
    return req;
  };
  auto serial = run(-2);
  ASSERT_OK(serial);
  glz::generic_sorted_u64 expected;
  ASSERT_FALSE(glz::read_json(expected, renderSearchResponseBody(serial->responses[0]->proto)));
  for (uint64_t value : {uint64_t{1}, uint64_t{10'000'000'000}, FANOUT_GRAIN_NEVER}) {
    FanoutGrainGuard grain(value);
    SCOPED_TRACE(value);
    TaskSpawnCounter counter;
    auto req = run(0);
    if (value == 1) { EXPECT_GT(counter.count(), 0); }
    else { EXPECT_EQ(0, counter.count()); }
    ASSERT_OK(req);
    ASSERT_EQ(1u, req->responses.size());
    glz::generic_sorted_u64 actual;
    ASSERT_FALSE(glz::read_json(actual, renderSearchResponseBody(req->responses[0]->proto)));
    EXPECT_EQ(glz::write_json(expected).value(), glz::write_json(actual).value());
  }
  // A single ranked op isolates collection spawns from exhaustive siblings.
  FanoutGrainGuard grain(1);
  for (std::string_view route : {"score", "column", "expression", "constant", "exact_count"}) {
    SCOPED_TRACE(route);
    auto req = localReq(helper.getSearchEngine());
    req->collection("fanout_gate");
    auto& top = req->topDocs("q").limit(7).fields({"id"});
    if (route == "constant") top.allQuery();
    // Keep exact-count membership cold after the field-sort cache routes.
    else top.matchQuery("body_w", route == "exact_count" ? "beta" : "alpha");
    if (route == "column") qb::sort(top, "value_i", qb::DESC);
    if (route == "expression") qb::sort(top, "value_i+1", qb::DESC);
    if (route == "exact_count") top.countLimit();
    TaskSpawnCounter counter;
    helper.getSearchEngine().dispatch(*req.get(), 0, SearchEngine::Receiver::MAY_EXECUTE);
    ASSERT_OK(req);
    if (route == "score") { EXPECT_GT(counter.count(), 0); }
    else { EXPECT_EQ(0, counter.count()); }
  }
}

}
