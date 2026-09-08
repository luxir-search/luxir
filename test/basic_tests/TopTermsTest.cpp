// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/search/TopTerms.h"

namespace luxir {

// Retention is clamp(above threshold, nTerms/64, maxEntries), so a test that
// wants the floor to do anything has to add enough terms to earn it.

TEST(TopTermsBuilderTest, KeepsEveryTermAboveThreshold) {
  TopTermsBuilder builder(10);
  const int64_t big[] = {20, 15, 30, 12, 11};
  for (int64_t i = 0; i < 5; i++) {
    builder.add(i, big[i]);
  }
  for (int64_t i = 5; i < 205; i++) {
    builder.add(i, 1);
  }

  TopTerms result = builder.finish();
  // 5 above the threshold beats the floor of 205/64 = 3.
  ASSERT_EQ(5u, result.entries.size());
  EXPECT_EQ(2, result.entries[0].ord);
  EXPECT_EQ(30, result.entries[0].df);
  EXPECT_EQ(4, result.entries[4].ord);
  EXPECT_EQ(11, result.entries[4].df);
  EXPECT_EQ(11, result.unlistedBound);
}

TEST(TopTermsBuilderTest, FloorCarriesAUniformField) {
  // Nothing clears the threshold - the case a percentage rule alone leaves
  // empty, which is exactly where the whole-domain fast path pays most.
  TopTermsBuilder builder(1000);
  for (int64_t ord = 0; ord < 640; ord++) {
    builder.add(ord, 1);
  }

  TopTerms result = builder.finish();
  ASSERT_EQ(10u, result.entries.size());
  for (int64_t i = 0; i < 10; i++) {
    EXPECT_EQ(i, result.entries[(size_t)i].ord);
    EXPECT_EQ(1, result.entries[(size_t)i].df);
  }
  EXPECT_EQ(1, result.unlistedBound);
}

TEST(TopTermsBuilderTest, TinyFieldKeepsNothing) {
  TopTermsBuilder builder(1000);
  for (int64_t ord = 0; ord < 10; ord++) {
    builder.add(ord, ord + 1);
  }

  TopTerms result = builder.finish();
  // Floor is zero, so nothing is retained and walking all ten was free anyway.
  EXPECT_TRUE(result.entries.empty());
  EXPECT_EQ(10, result.unlistedBound);
}

TEST(TopTermsBuilderTest, WholeFieldRetainedBoundsNothing) {
  TopTermsBuilder builder(0, 8);
  builder.add(0, 3);
  builder.add(1, 9);
  builder.add(2, 5);

  TopTerms result = builder.finish();
  ASSERT_EQ(3u, result.entries.size());
  EXPECT_EQ(1, result.entries[0].ord);
  EXPECT_EQ(2, result.entries[1].ord);
  EXPECT_EQ(0, result.entries[2].ord);
  EXPECT_EQ(0, result.unlistedBound);
}

TEST(TopTermsBuilderTest, CapBoundsRetention) {
  TopTermsBuilder builder(0, 8);
  for (int64_t ord = 0; ord < 100; ord++) {
    builder.add(ord, ord + 1);
  }

  TopTerms result = builder.finish();
  ASSERT_EQ(8u, result.entries.size());
  EXPECT_EQ(99, result.entries[0].ord);
  EXPECT_EQ(100, result.entries[0].df);
  EXPECT_EQ(92, result.entries[7].ord);
  EXPECT_EQ(93, result.entries[7].df);
  EXPECT_EQ(93, result.unlistedBound);
}

TEST(TopTermsBuilderTest, SmallerOrdWinsRetentionBoundaryTie) {
  TopTermsBuilder builder(0, 2);
  builder.add(0, 5);
  builder.add(1, 4);
  builder.add(2, 4);

  TopTerms result = builder.finish();
  ASSERT_EQ(2u, result.entries.size());
  EXPECT_EQ(0, result.entries[0].ord);
  EXPECT_EQ(1, result.entries[1].ord);
  // An evicted term can hold exactly this df, so the certificate needs a
  // strict comparison against it.
  EXPECT_EQ(4, result.unlistedBound);
}

TEST(TopTermsBuilderTest, SkipsEmptyTerms) {
  TopTermsBuilder builder(0, 8);
  builder.add(0, 2);
  builder.add(1, 0);
  builder.add(2, 5);

  TopTerms result = builder.finish();
  ASSERT_EQ(2u, result.entries.size());
  EXPECT_EQ(2, result.entries[0].ord);
  EXPECT_EQ(0, result.entries[1].ord);
}

}
