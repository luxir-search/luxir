// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <algorithm>
#include <initializer_list>
#include <random>
#include <vector>
#include "luxir/util/BranchlessSearch.h"
#include "luxir/util/screaming.h"
#include "test/LuxirTest.h"

using namespace luxir;
using screaming::gallopLowerBound;

// Cheap fuzz check that the production gallop and branchless searches
// match std::lower_bound across edge-case lengths and every key position.
TEST(BranchlessSearchTest, gallopAndBranchlessMatchStd) {
  auto& rng = LuxirTest::rng;  // gtest-seeded (per-test) fast RNG
  for (size_t len : std::initializer_list<size_t>{0, 1, 2, 3, 7, 16, 100, 4096}) {
    std::uniform_int_distribution<int> v(0, (int)len + 5);
    std::vector<uint16_t> arr(len);
    for (auto& x : arr) x = (uint16_t)v(rng);
    std::sort(arr.begin(), arr.end());
    for (int t = -1; t <= (int)len + 6; t++) {
      uint16_t key = (uint16_t)std::max(0, t);
      auto* expect = std::lower_bound(arr.data(), arr.data() + len, key);
      EXPECT_EQ(gallopLowerBound(arr.data(), arr.data() + len, key), expect) << "gallop len=" << len << " key=" << key;
      EXPECT_EQ(BranchlessIndex<uint16_t>::lowerBound(arr.data(), len, key), expect) << "branchless len=" << len << " key=" << key;
    }
  }
}
