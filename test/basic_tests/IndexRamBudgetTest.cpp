// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>

#include "luxir/index/IndexRamBudget.h"

using namespace luxir;

TEST(IndexRamBudgetTest, UnlimitedBudgetAlwaysAcquires) {
  IndexRamBudget budget;

  EXPECT_TRUE(budget.tryAcquire(1024));
  EXPECT_TRUE(budget.tryAcquire(2048));
  EXPECT_EQ(3072, budget.reservedBytes());

  budget.release(1024);
  budget.release(2048);
  EXPECT_EQ(0, budget.reservedBytes());
}

TEST(IndexRamBudgetTest, TryAcquireRespectsTotal) {
  IndexRamBudget budget(10);

  EXPECT_TRUE(budget.tryAcquire(6));
  EXPECT_FALSE(budget.tryAcquire(5));
  EXPECT_EQ(6, budget.reservedBytes());

  budget.release(6);
  EXPECT_TRUE(budget.tryAcquire(10));
  budget.release(10);
}

TEST(IndexRamBudgetTest, ForceAcquireAllowsBoundedOverdraft) {
  IndexRamBudget budget(10);

  auto guard = budget.forceAcquire(25);
  EXPECT_EQ(25, budget.reservedBytes());
  EXPECT_FALSE(budget.tryAcquire(1));

  guard.release();
  EXPECT_EQ(0, budget.reservedBytes());
}

TEST(IndexRamBudgetTest, GuardReleasesOnDestructionAndMove) {
  IndexRamBudget budget(10);

  {
    auto guard = budget.tryAcquireGuard(7);
    ASSERT_TRUE(guard.has_value());
    EXPECT_EQ(7, budget.reservedBytes());

    IndexRamBudget::Guard moved = std::move(*guard);
    EXPECT_EQ(7, budget.reservedBytes());
  }

  EXPECT_EQ(0, budget.reservedBytes());
  EXPECT_TRUE(budget.tryAcquire(10));
  budget.release(10);
}

TEST(IndexRamBudgetTest, GuardForceResizeReportsOverBudget) {
  IndexRamBudget budget(10);
  IndexRamBudget::Guard guard(budget, 0);

  EXPECT_FALSE(guard.forceResize(8));
  EXPECT_EQ(8, budget.reservedBytes());
  EXPECT_TRUE(guard.forceResize(25)); // overdraws like forceAcquire
  EXPECT_EQ(25, budget.reservedBytes());
  EXPECT_FALSE(guard.forceResize(3));
  EXPECT_EQ(3, budget.reservedBytes());

  guard.release();
  EXPECT_EQ(0, budget.reservedBytes());

  IndexRamBudget uncapped;
  IndexRamBudget::Guard unGuard(uncapped, 0);
  EXPECT_FALSE(unGuard.forceResize(1 << 30)); // uncapped never reports over
}

TEST(IndexRamBudgetTest, GuardResizeIsAtomic) {
  IndexRamBudget budget(10);
  auto guard = budget.tryAcquireGuard(4);
  ASSERT_TRUE(guard.has_value());

  EXPECT_TRUE(guard->tryResize(8));
  EXPECT_EQ(8, budget.reservedBytes());
  EXPECT_FALSE(guard->tryResize(11));
  EXPECT_EQ(8, budget.reservedBytes());
  EXPECT_TRUE(guard->tryResize(3));
  EXPECT_EQ(3, budget.reservedBytes());
}

TEST(IndexRamBudgetTest, MergeDemandAggregatesCapsAndUnregisters) {
  IndexRamBudget budget(10);
  std::atomic<int> pressurePings = 0;
  auto pressure = budget.registerPressureListener([&]() {
    pressurePings.fetch_add(1, std::memory_order_relaxed);
  });
  auto first = budget.registerMergeDemand([]() {});
  auto second = budget.registerMergeDemand([]() {});
  auto guard = budget.tryAcquireGuard(2);
  ASSERT_TRUE(guard.has_value());

  first.publish(7);
  EXPECT_EQ(7, budget.pendingMergeDemandBytes());
  EXPECT_FALSE(budget.pressureNeeded());

  second.publish(8);
  EXPECT_EQ(10, budget.pendingMergeDemandBytes());
  EXPECT_TRUE(budget.pressureNeeded());
  EXPECT_GT(pressurePings.load(std::memory_order_relaxed), 0);

  first.reset();
  EXPECT_EQ(8, budget.pendingMergeDemandBytes());
  second.publish(0);
  EXPECT_EQ(0, budget.pendingMergeDemandBytes());
  EXPECT_FALSE(budget.pressureNeeded());
}

TEST(IndexRamBudgetTest, DrainingDiscountsPressureUntilReservationRelease) {
  IndexRamBudget budget(10);
  std::atomic<int> admissionPings = 0;
  auto demand = budget.registerMergeDemand([&]() {
    admissionPings.fetch_add(1, std::memory_order_relaxed);
    EXPECT_FALSE(budget.pressureNeeded());
  });
  auto guard = budget.tryAcquireGuard(10);
  ASSERT_TRUE(guard.has_value());

  demand.publish(5);
  EXPECT_TRUE(budget.pressureNeeded());
  EXPECT_TRUE(guard->tryMarkDrainingForPressure());
  EXPECT_EQ(10, budget.reservedBytes());
  EXPECT_EQ(10, budget.drainingBytesCount());
  EXPECT_FALSE(budget.pressureNeeded());

  guard->release();
  EXPECT_EQ(0, budget.reservedBytes());
  EXPECT_EQ(0, budget.drainingBytesCount());
  EXPECT_EQ(1, admissionPings.load(std::memory_order_relaxed));
}
