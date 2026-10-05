// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <thread>
#include "luxir/util/AtomicSharedPtr.h"

namespace luxir {

TEST(AtomicSharedPtrTest, LoadedSnapshotSurvivesRetirement) {
  auto initial = std::make_shared<int>(7);
  std::weak_ptr<int> lifetime = initial;
  AtomicSharedPtr<int> value(std::move(initial));
  auto snapshot = value.load(std::memory_order_acquire);
  auto retired = value.exchange(nullptr, std::memory_order_acq_rel);
  EXPECT_EQ(snapshot, retired);
  retired.reset();
  EXPECT_FALSE(lifetime.expired());
  EXPECT_EQ(7, *snapshot);
  snapshot.reset();
  EXPECT_TRUE(lifetime.expired());
}

TEST(AtomicSharedPtrTest, CompareExchangeUpdatesExpectedOnFailure) {
  auto initial = std::make_shared<int>(7);
  AtomicSharedPtr<int> value(initial);
  auto expected = std::make_shared<int>(8);
  EXPECT_FALSE(value.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire));
  EXPECT_EQ(initial, expected);
  EXPECT_TRUE(value.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire));
  EXPECT_EQ(nullptr, value.load());
}

TEST(AtomicSharedPtrTest, ConcurrentLoadsSeeCompletePublishedValues) {
  using Payload = std::pair<int, int>;
  AtomicSharedPtr<const Payload> value(std::make_shared<const Payload>(0, 0));
  std::atomic<bool> ready{false};
  std::atomic<int> errors{0};
  std::thread reader([&] {
    ready.store(true, std::memory_order_release);
    while (auto snapshot = value.load(std::memory_order_acquire)) {
      if (snapshot->first != snapshot->second) errors.fetch_add(1);
    }
  });
  while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
  for (int i = 1; i <= 1000; ++i) {
    value.store(std::make_shared<const Payload>(i, i), std::memory_order_release);
  }
  value.store(nullptr, std::memory_order_release);
  reader.join();
  EXPECT_EQ(0, errors.load());
}

}  // namespace luxir
