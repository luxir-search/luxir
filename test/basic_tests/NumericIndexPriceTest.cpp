// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "luxir/query/NumericIndexPrice.h"

using namespace luxir;

TEST(NumericIndexPriceTest, encodedPointsAndComplementTotals) {
  PointsReader::EmissionWork work{3, 6400, 200, 10, 4, 2};
  // 1000 destination words, three leaves, decode/scatter, source/run words,
  // and two bounded bitmap selections. Complement also fills the base set.
  EXPECT_DOUBLE_EQ(3422, NumericIndexPrice::points(work, 1000, 64000));
  EXPECT_DOUBLE_EQ(3662, NumericIndexPrice::points(work, 1000, 64000, true, 100));
  EXPECT_DOUBLE_EQ(5000, NumericIndexPrice::points(work, 500, 64000));
}

TEST(NumericIndexPriceTest, ordinalLowerBoundIncludesRepeatedBoundaryLeaves) {
  const PointsReader::OrdinalRange ranges[] = {{1, 4}, {7, 9}};
  EXPECT_DOUBLE_EQ(39, NumericIndexPrice::pointsLowerBound(ranges, 12, 3, 5, 64));
  EXPECT_DOUBLE_EQ(51.15, NumericIndexPrice::pointsLowerBound(ranges, 12, 3, 7, 64, true));
  EXPECT_DOUBLE_EQ(50, NumericIndexPrice::pointsLowerBound(ranges, 12, 3, 5, 6400));
}
