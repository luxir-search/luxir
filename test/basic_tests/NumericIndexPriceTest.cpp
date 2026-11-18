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

TEST(NumericIndexPriceTest, postingsChargeEachTermsTailAsPacked) {
  NumericIndexPrice::PostingsWork work;
  work.add(256, 2, 1 << 20);
  work.add(256, 0, 1 << 20);
  work.add(63, 0, 1 << 20);
  work.add(65, 0, 1 << 20);
  EXPECT_EQ(384, work.packedDocs);
  EXPECT_EQ(2, work.wordBlocks);
  EXPECT_DOUBLE_EQ(16384, work.spanWords);
  EXPECT_DOUBLE_EQ(165504, NumericIndexPrice::postings(work, 1 << 20));
  EXPECT_DOUBLE_EQ(3000, NumericIndexPrice::postingsUnion(1000, 64000));
  EXPECT_DOUBLE_EQ(5000, NumericIndexPrice::postingsUnion(500, 64000));
}

TEST(NumericIndexPriceTest, shuffledBooleanAndDateGates) {
  constexpr int32_t N = 1 << 20;
  double verify = PointsMaterialize::VERIFY_POINTS * (N / 100);
  PointsReader::EmissionWork bits{1024, 0, 0, 17400, 0, 0};
  PointsReader::EmissionWork randomDates{1024, (uint64_t)N / 2 * 20, N / 2, 0, 0, 0};
  EXPECT_LT(NumericIndexPrice::points(bits, N / 2, N), verify);
  EXPECT_GT(NumericIndexPrice::points(randomDates, N / 2, N), verify);
  for (int density : {10, 50, 90}) {
    uint64_t docs = (uint64_t)N * density / 100;
    NumericIndexPrice::PostingsWork work;
    work.add(docs, density == 10 ? docs / 128 : 0, N);
    double price = NumericIndexPrice::postings(work, N);
    if (density == 10) EXPECT_GT(price, verify);
    else EXPECT_LT(price, verify);
  }
}

TEST(NumericIndexPriceTest, postingsSpanUsesEachTermsDensity) {
  NumericIndexPrice::PostingsWork work;
  work.add(512, 0, 1024); // Four half-density blocks span 16 words.
  work.add(256, 1, 1024); // One quarter-density word block spans 8 words.
  EXPECT_DOUBLE_EQ(24, work.spanWords);
  EXPECT_EQ(5, work.wordBlocks);
  EXPECT_DOUBLE_EQ(640, NumericIndexPrice::postings(work, 1024));
}

TEST(NumericIndexPriceTest, termBoundsAllowCollisionsAndWideDomains) {
  EXPECT_EQ(100, NumericIndexPrice::postingsTermLowerBound(100, 1000, 1000));
  EXPECT_EQ(80, NumericIndexPrice::postingsTermLowerBound(100, 1000, 980));
  EXPECT_EQ(0, NumericIndexPrice::postingsTermLowerBound(100, 1000, 800));
  EXPECT_EQ(500, NumericIndexPrice::postingsDomainLowerBound(1000, 0, 999, 250, 749));
  EXPECT_EQ(0, NumericIndexPrice::postingsDomainLowerBound(1000, 0, 999'000, 250'000, 749'000));
  EXPECT_EQ(0, NumericIndexPrice::postingsDomainLowerBound(1000, INT64_MIN, INT64_MAX, -1, 1));
  EXPECT_EQ(999, NumericIndexPrice::postingsDomainLowerBound(
      1000, INT64_MIN, INT64_MAX, INT64_MIN + 1, INT64_MAX));
}
