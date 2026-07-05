#include <gtest/gtest.h>

#include "solux/index/IndexRamBudget.h"

using namespace solux;

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
