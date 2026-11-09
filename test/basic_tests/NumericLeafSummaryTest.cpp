// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "luxir/reader/IntColReader.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class NumericLeafSummaryTest : public LuxirTest {
protected:
  static constexpr int64_t LEAF = NumColumnFormat::LEAF_ZONE_SIZE;
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;

  void SetUp() override {
    // Below, left-crossing, spanning, inside, right-crossing, above, then
    // a partial inside leaf for [-5, 5]. Include the int64 endpoints.
    const int64_t pairs[][2] = {
        {INT64_MIN, -10}, {-10, 0}, {-10, 10}, {0, 1}, {0, 10}, {10, INT64_MAX}};
    std::vector<Doc> docs;
    for (int64_t doc = 0; doc < 6 * LEAF + 37; doc++) {
      int64_t value = doc < 6 * LEAF ? pairs[doc / LEAF][doc % 2] : 3;
      docs.push_back(flatdoc("id", std::to_string(doc), "value_i", value));
    }
    std::vector<int64_t> many(1100);
    std::iota(many.begin(), many.end(), 0);
    docs[0].push_back({"multi_is", std::move(many)});
    docs[1].push_back({"multi_is", vec_i(-100)});
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(1u, reader->segments().size());
    for (auto field : {"value_i", "multi_is"}) {
      ASSERT_NE(nullptr, reader->segments()[0].postingsReader().fieldInfo(field));
    }
  }

  IntColReader column(std::string_view field) {
    auto& postings = reader->segments()[0].postingsReader();
    return IntColReader(postings, *postings.fieldInfo(field));
  }

  static auto check(IntColReader& column, int64_t lo, int64_t hi) {
    SCOPED_TRACE(::testing::Message() << lo << ".." << hi);
    const auto& leaves = column.leafMeta();
    auto summary = leaves.summarizeRange(lo, hi);
    int64_t outside = 0, inside = 0, matches = 0;
    for (int64_t leaf = 0; leaf < leaves.numLeafZones(); leaf++) {
      auto zone = leaves.leafZone(leaf);
      int64_t count = std::min(LEAF, column.numValues() - leaf * LEAF);
      if (zone.max < lo || zone.min > hi) outside += count;
      if (zone.min >= lo && zone.max <= hi) inside += count;
    }
    IntColReader::SparseValues values(column);
    for (int64_t rank = 0; rank < column.numValues(); rank++) {
      int64_t value = values.valueAt(rank);
      matches += lo <= value && value <= hi;
    }
    EXPECT_EQ(outside, summary.outside);
    EXPECT_LE(0, summary.insideLower);
    EXPECT_LE(summary.insideLower, inside);
    EXPECT_LE(inside, summary.insideUpper);
    EXPECT_LE(summary.insideUpper, column.numValues() - outside);
    EXPECT_LE(summary.insideLower, matches);
    EXPECT_LE(matches, column.numValues() - summary.outside);
    return summary;
  }
};

TEST_F(NumericLeafSummaryTest, oneSidedAndEndpointEquality) {
  auto values = column("value_i");
  for (auto [lo, hi] : std::array<std::pair<int64_t, int64_t>, 7>{{
           {INT64_MIN, -10}, {INT64_MIN, 0}, {0, INT64_MAX}, {10, INT64_MAX},
           {INT64_MIN, INT64_MAX}, {INT64_MIN, INT64_MIN}, {INT64_MAX, INT64_MAX}}}) {
    auto summary = check(values, lo, hi);
    EXPECT_EQ(summary.insideLower, summary.insideUpper);
  }
  auto empty = NumColumnFormat::LeafMeta{}.summarizeRange(INT64_MIN, INT64_MAX);
  EXPECT_EQ(0, empty.outside);
  EXPECT_EQ(0, empty.insideLower);
  EXPECT_EQ(0, empty.insideUpper);
}

TEST_F(NumericLeafSummaryTest, boundedIntervalsAndPartialLeaf) {
  auto values = column("value_i");
  auto middle = check(values, -5, 5);
  EXPECT_EQ(2 * LEAF, middle.outside);
  EXPECT_EQ(37, middle.insideLower);
  EXPECT_EQ(2 * LEAF + 37, middle.insideUpper);
  for (auto [lo, hi] : std::array<std::pair<int64_t, int64_t>, 4>{{
           {0, 1}, {3, 3}, {4, 5}, {-10, 10}}}) {
    check(values, lo, hi);
  }
  // Only the short final leaf is wholly inside this singleton interval.
  auto tail = check(values, 3, 3);
  EXPECT_EQ(37, tail.insideUpper);
}

TEST_F(NumericLeafSummaryTest, multivaluedCountsAreValueRanks) {
  auto values = column("multi_is");
  ASSERT_TRUE(values.multiValued());
  ASSERT_EQ(1101, values.numValues());
  ASSERT_EQ(2, values.docsWithValue());
  auto broad = check(values, 0, 1099);
  EXPECT_EQ(2 * LEAF, broad.insideLower);
  EXPECT_GT(broad.insideLower, values.docsWithValue());

  auto tail = check(values, 1024, 1099);
  EXPECT_EQ(2 * LEAF, tail.outside);
  EXPECT_GT(tail.outside, values.docsWithValue());
  // Both ranges match one document. Neither value count can be used as a
  // distinct-document lower bound, or subtracted from docsWithValue().
  IntColReader::SparseValues raw(values);
  for (int64_t lo : {0, 1024}) {
    int64_t matchedDocs = 0;
    for (int32_t doc = 0; doc < values.docsWithValue(); doc++) {
      auto [begin, end] = values.getStartEndValueRank(doc);
      bool matches = false;
      for (int64_t rank = begin; rank < end; rank++) {
        int64_t value = raw.valueAt(rank);
        matches |= lo <= value && value <= 1099;
      }
      matchedDocs += matches;
    }
    EXPECT_EQ(1, matchedDocs);
  }
}

} // namespace
