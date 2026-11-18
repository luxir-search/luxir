// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "luxir/util/MemPool.h"
#include "luxir/index/OrdCollector.h"

using namespace std;
using namespace luxir;

class OrdCollectorTest : public luxir::LuxirTest {
protected:
};


TEST_F(OrdCollectorTest, basic) {
  MemPool pool;
  OrdCollector oc(pool, 5);
  oc.add(1,7);
  oc.add(2, 11);
  oc.add(1, 13);
  oc.add(4, 17);
  oc.add(1, 19);

  ASSERT_TRUE(oc.multiValued());

  ASSERT_FALSE(oc.hasValues(0));
  ASSERT_TRUE(oc.hasValues(1));
  ASSERT_TRUE(oc.hasValues(2));
  ASSERT_FALSE(oc.hasValues(3));
  ASSERT_TRUE(oc.hasValues(4));

  vector<int32_t> ords;
  oc.pushValues(1, [&](int32_t ord) { ords.push_back(ord); });
  ASSERT_EQ(ords, (std::vector<int32_t>{7, 13, 19}));
  ords.clear();
  oc.pushValues(2, [&](int32_t ord) { ords.push_back(ord); });
  ASSERT_EQ(ords, (std::vector<int32_t>{11}));
  ords.clear();
  oc.pushValues(3, [&](int32_t ord) { ords.push_back(ord); });
  ASSERT_EQ(ords, (std::vector<int32_t>{}));
  ords.clear();
  oc.pushValues(4, [&](int32_t ord) { ords.push_back(ord); });
  ASSERT_EQ(ords, (std::vector<int32_t>{17}));
}

// A stream placed past 2 GiB cannot carry the tag bit; the collector must fail
// rather than tag it with the top bit cleared and later read 2 GiB lower.
TEST_F(OrdCollectorTest, streamPastTagBitThrows) {
#ifndef NDEBUG
  GTEST_SKIP() << "debug pools scribble every allocation, which would touch 2 GiB";
#endif
  MemPool pool;
  OrdCollector oc(pool, 2);
  oc.add(0, 1);
  oc.add(0, 2);  // a stream below 2 GiB still works
  // Release pools leave unwritten blocks untouched, so this is cheap.
  while (pool.bbAddress() < 0x80000000u) pool.allocateBBP(MemPool::BYTE_BLOCK_SIZE / 2);
  oc.add(1, 1);
  EXPECT_THROW(oc.add(1, 2), std::length_error);
  oc.add(0, 3);
  std::vector<int32_t> ords;
  oc.pushValues(0, [&](int32_t ord) { ords.push_back(ord); });
  EXPECT_EQ((std::vector<int32_t>{1, 2, 3}), ords);
}
