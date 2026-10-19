// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <random>
#include <set>

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/TestUtils.h"
#include "luxir/search/ops/DistinctFacetCounts.h"
#include "luxir/util/ApiError.h"
#include "test/RangeFacetTestUtils.h"

using namespace luxir;
using namespace luxir::test;
namespace {
using Mode = DistinctFacetMode;
using Grid = RangeFacetGrid;
using Counts = DistinctFacetCounts;
using Pairs = std::map<int64_t, std::set<std::pair<int, int64_t>>>;

void add(Counts& counts, int64_t value, GroupIdentity group = {GroupIdentity::VALUE, 0}) {
  counts.collectValues([&](auto add, auto) { add(value, group); });
}
uint64_t** rowAt(const Counts& counts, int64_t key) {
  auto* window = counts.cellWindow();
  for (size_t i = 0; i < window->capacity(); i++) if (window->cellKey(i) == key) return window->entries()[i];
  return nullptr;
}
void check(Counts& counts, const Pairs& expected, const std::set<std::pair<int, int64_t>>& missing) {
  auto bounds = counts.cellWindow()->occupiedBounds();
  ASSERT_EQ(!expected.empty(), bounds.has_value());
  if (bounds) {
    EXPECT_EQ(expected.begin()->first, bounds->first);
    EXPECT_EQ(expected.rbegin()->first, bounds->second);
  }
  counts.finish();
  for (const auto& [key, groups] : expected) EXPECT_EQ((int64_t)groups.size(), counts.count(key)) << key;
  for (const auto& [key, count] : counts.counts) {
    ASSERT_TRUE(expected.contains(key));
    EXPECT_EQ((int64_t)expected.at(key).size(), count);
  }
  EXPECT_EQ((int64_t)missing.size(), counts.missing());
}

// Bounded inputs and contributor counts; no random termination conditions.
void randomized(const Grid& grid, std::vector<int64_t> values) {
  std::mt19937 random(57);
  for (bool ordered : {true, false}) {
    if (ordered) std::sort(values.begin(), values.end());
    else std::shuffle(values.begin(), values.end(), random);
    for (Mode mode : {Mode::PAGED, Mode::HASH, Mode::AUTO}) {
      for (size_t splits : {size_t{1}, size_t{3}, size_t{8}}) {
        RequestMemTracker tracker(0);
        {
          std::vector<std::unique_ptr<Counts>> contributors;
          for (size_t i = 0; i < splits; i++) {
            // AUTO alternates adaptive PAGED and HASH to exercise mixed unions.
            Mode requested = mode == Mode::AUTO && i % 2 ? Mode::HASH : mode;
            contributors.push_back(std::make_unique<Counts>(tracker, "oracle", requested, grid,
                FacetDimension{true, -123, 4096}, 4096));
          }
          Pairs expected;
          std::set<std::pair<int, int64_t>> missing;
          for (size_t i = 0; i < values.size(); i++) {
            size_t split = ordered ? std::min(splits - 1, i * splits / values.size()) : i % splits;
            auto kind = i % 11 == 0 ? GroupIdentity::DROP : i % 7 == 0 ? GroupIdentity::NULL_KEY
                : i % 5 == 0 ? GroupIdentity::SINGLETON : GroupIdentity::VALUE;
            int64_t id = kind == GroupIdentity::SINGLETON ? (int64_t)i : (int64_t)(random() % 4096) - 123;
            GroupIdentity group{kind, id};
            int64_t key = grid.floor(values[i]).key;
            auto& wanted = expected[key]; // Raw DROP observations still occupy.
            if (kind != GroupIdentity::DROP) wanted.emplace((int)kind, kind == GroupIdentity::NULL_KEY ? 0 : id);
            if (i % 9 == 0 && kind != GroupIdentity::DROP) missing.emplace((int)kind, kind == GroupIdentity::NULL_KEY ? 0 : id);
            contributors[split]->collectValues([&](auto add, auto absent) {
              add(values[i], group); add(values[i], group); // Includes singleton per-document deduplication.
              if (i % 9 == 0) { absent(group); absent(group); }
            });
          }
          Counts* merged = contributors[0].get();
          for (size_t i = 1; i < splits; i++) merged = Counts::merge(merged, contributors[i].get());
          check(*merged, expected, missing);
        }
        EXPECT_EQ(0u, tracker.bytes());
      }
    }
  }
}
}

TEST(GrowingDistinctFacetTest, affineRandomizedPairOracle) {
  auto grid = Grid::fixed(FieldType::Type::INT, 7, 3);
  std::mt19937 random(123);
  std::vector<int64_t> values(300);
  for (auto& value : values) value = (int64_t)(random() % 1400) - 700;
  randomized(grid, values);
}

TEST(GrowingDistinctFacetTest, nonAffineRandomizedPairOracle) {
  std::mt19937 random(123);
  for (auto grid : {Grid::fixed(FieldType::Type::DATE, 45 * 60000, 17 * 60000,
                               resolveTimeZone("America/New_York").value()),
                    Grid::calendar(CalendarUnit::DAY, 1, resolveTimeZone("Pacific/Apia").value())}) {
    std::vector<int64_t> values(180);
    int64_t base = grid.timeZone().name() == "Pacific/Apia" ? epoch("2011-12-30") : epoch("2024-11-03");
    for (auto& value : values) value = base + (int64_t)(random() % (5 * 86400000)) - 2 * 86400000;
    randomized(grid, values);
  }
}

TEST(GrowingDistinctFacetTest, floatingCanonicalKeysPairOracle) {
  for (auto type : {FieldType::Type::FLOAT, FieldType::Type::DOUBLE}) {
    auto grid = Grid::floating(type, 0.1, 0.07);
    std::mt19937 random(123);
    std::vector<int64_t> values(180);
    for (auto& value : values) value = grid.encode(((int)(random() % 2000) - 1000) / 100.0);
    randomized(grid, values);
  }
}

TEST(GrowingDistinctFacetTest, stablePagesAndSeparateMissingRowAcrossBothEnds) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(0);
  {
    Counts counts(tracker, "stable", Mode::PAGED, grid, {true, 0, 1000000});
    counts.collectValues([](auto add, auto missing) { add(0, 17); missing(23); });
    auto** row = rowAt(counts, 0);
    ASSERT_NE(nullptr, row);
    auto* page = row[0];
    for (int64_t i = 1; i < 100; i++) {
      add(counts, i % 2 ? -i : i);
      EXPECT_EQ(row, rowAt(counts, 0));
      EXPECT_EQ(page, rowAt(counts, 0)[0]);
    }
    counts.finish();
    EXPECT_EQ(1, counts.count(0));
    EXPECT_EQ(1, counts.missing());
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST(GrowingDistinctFacetTest, observedDimensionsAndOneWayCrossover) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(0);
  Counts narrow(tracker, "narrow", Mode::AUTO, grid, {true, 0, 1000000});
  for (int64_t i = 0; i < 4; i++) add(narrow, i, {GroupIdentity::VALUE, i * 4096});
  EXPECT_EQ(Mode::PAGED, narrow.selectedMode());
  Counts sparse(tracker, "sparse", Mode::AUTO, grid, {true, 0, 4096}, 4096);
  sparse.collectValues([](auto add, auto missing) {
    missing(19);
    for (int64_t i = 0; i < 200; i++) add(i, i);
  });
  EXPECT_EQ(Mode::HASH, sparse.selectedMode());
  EXPECT_EQ(1u, sparse.conversionCount());
  sparse.finish();
  for (int64_t i = 0; i < 200; i++) EXPECT_EQ(1, sparse.count(i));
  EXPECT_EQ(1, sparse.missing());
  Counts dense(tracker, "dense", Mode::AUTO, grid, {true, 0, 4096}, 1024);
  dense.collectValues([](auto add, auto) {
    for (int64_t cell = 0; cell < 32; cell++) for (int64_t group = 0; group < 4096; group++) add(cell, group);
  });
  EXPECT_EQ(Mode::PAGED, dense.selectedMode());
  EXPECT_EQ(0u, dense.conversionCount());
  dense.finish();
  EXPECT_EQ(4096, dense.count(31));
  Counts moderate(tracker, "moderate", Mode::AUTO, grid, {true, 0, 4096}, 4096);
  moderate.collectValues([](auto add, auto) {
    for (int64_t cell = 0; cell < 200; cell++) { add(cell, 0); add(cell, 1); }
  });
  EXPECT_EQ(Mode::PAGED, moderate.selectedMode());
  EXPECT_EQ(0u, moderate.conversionCount());
}

TEST(GrowingDistinctFacetTest, pagedHashAndMixedMergeBothOrders) {
  auto grid = Grid::fixed(FieldType::Type::INT, 7, 3);
  for (Mode first : {Mode::PAGED, Mode::HASH}) for (Mode second : {Mode::PAGED, Mode::HASH}) {
    for (bool reverse : {false, true}) {
      RequestMemTracker tracker(0);
      Counts a(tracker, "a", first, grid, {true, -100, 8192});
      Counts b(tracker, "b", second, grid, {true, -100, 8192});
      a.collectValues([](auto add, auto missing) { add(-70, -90); add(7, 4000); missing(10); });
      b.collectValues([](auto add, auto missing) { add(7, 4000); add(7, 8000); add(700, -90); missing(10); missing(11); });
      auto* merged = reverse ? Counts::merge(&b, &a) : Counts::merge(&a, &b);
      merged->finish();
      EXPECT_EQ(1, merged->count(grid.floor(-70).key));
      EXPECT_EQ(2, merged->count(grid.floor(7).key));
      EXPECT_EQ(1, merged->count(grid.floor(700).key));
      EXPECT_EQ(2, merged->missing());
    }
  }
}

TEST(GrowingDistinctFacetTest, conversionDuringPagedMerge) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(0);
  Counts a(tracker, "adaptive", Mode::AUTO, grid, {true, 0, 4096}, 4096);
  Counts b(tracker, "source", Mode::PAGED, grid, {true, 0, 4096});
  add(a, 0); add(a, 64, {GroupIdentity::DROP});
  b.collectValues([](auto add, auto missing) {
    for (int64_t i = 1; i < 61; i++) add(i, i);
    missing(23);
  });
  EXPECT_EQ(&a, Counts::merge(&a, &b));
  EXPECT_EQ(Mode::HASH, a.selectedMode());
  EXPECT_EQ(1u, a.conversionCount());
  a.finish();
  for (int64_t i = 0; i < 61; i++) EXPECT_EQ(1, a.count(i));
  EXPECT_EQ(0, a.count(64));
  EXPECT_EQ(1, a.missing());
}

TEST(GrowingDistinctFacetTest, foldOccurrencesAndSkippedDayMergeByInstant) {
  for (bool reverse : {false, true}) {
    for (bool skipped : {false, true}) {
      auto grid = skipped ? Grid::calendar(CalendarUnit::DAY, 1, resolveTimeZone("Pacific/Apia").value())
          : Grid::fixed(FieldType::Type::DATE, 3600000, 17 * 60000, resolveTimeZone("America/New_York").value());
      int64_t first = epoch(skipped ? "2011-12-30T09:59Z" : "2024-11-03T05:20Z");
      int64_t second = epoch(skipped ? "2011-12-30T10:01Z" : "2024-11-03T06:20Z");
      auto key = grid.floor(first), following = grid.next(key);
      EXPECT_EQ(following.key, grid.floor(second).key);
      RequestMemTracker tracker(0);
      Counts a(tracker, "before", Mode::PAGED, grid, {true, 0, 64});
      Counts b(tracker, "after", Mode::PAGED, grid, {true, 0, 64});
      add(a, first); add(b, second);
      auto* merged = reverse ? Counts::merge(&b, &a) : Counts::merge(&a, &b);
      EXPECT_EQ(2u, merged->cellWindow()->size());
      merged->finish();
      EXPECT_EQ(1, merged->count(key.key));
      EXPECT_EQ(1, merged->count(following.key));
    }
  }
}

TEST(GrowingDistinctFacetTest, missingOnlyContributorsAndGrowingRun) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  for (Mode mode : {Mode::PAGED, Mode::HASH, Mode::RUN}) {
    RequestMemTracker tracker(0);
    Counts a(tracker, "a", mode, grid, {true, 0, 64}, 4096, 100000, mode == Mode::RUN);
    Counts b(tracker, "b", mode, grid, {true, 0, 64}, 4096, 100000, mode == Mode::RUN);
    a.collectValues([](auto, auto missing) { missing(1); missing(1); });
    b.collectValues([](auto, auto missing) { missing(2); missing(2); });
    auto* merged = Counts::merge(&a, &b);
    EXPECT_TRUE(merged->cellWindow()->empty());
    if (mode == Mode::RUN) merged->collectValues([](auto add, auto) {
      add(-10, 1); add(-10, 1); add(-10, 2); add(10, 2);
    });
    merged->finish();
    EXPECT_EQ(2, merged->missing());
    if (mode == Mode::RUN) { EXPECT_EQ(2, merged->count(-10)); EXPECT_EQ(1, merged->count(10)); }
  }
}

TEST(GrowingDistinctFacetTest, taggedPoliciesDoNotCollideWithOrdinaryGroups) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  for (Mode mode : {Mode::PAGED, Mode::HASH}) {
    RequestMemTracker tracker(0);
    Counts counts(tracker, "tags", mode, grid, {true, 0, 64});
    counts.collectValues([](auto add, auto missing) {
      for (auto group : {GroupIdentity{GroupIdentity::VALUE, 0}, GroupIdentity{GroupIdentity::NULL_KEY},
                         GroupIdentity{GroupIdentity::SINGLETON, 0}, GroupIdentity{GroupIdentity::DROP}}) {
        add(0, group); add(0, group); missing(group); missing(group);
      }
      add(-20, GroupIdentity{GroupIdentity::DROP});
      add(20, GroupIdentity{GroupIdentity::DROP});
    });
    EXPECT_EQ((std::pair<int64_t, int64_t>{-20, 20}), *counts.cellWindow()->occupiedBounds());
    counts.finish();
    EXPECT_EQ(3, counts.count(0));
    EXPECT_EQ(0, counts.count(-20));
    EXPECT_EQ(3, counts.missing());
  }
}

TEST(GrowingDistinctFacetTest, capsIncludeDropGrowthMergeAndWidening) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  for (Mode mode : {Mode::PAGED, Mode::HASH}) {
    for (bool reverse : {false, true}) {
      RequestMemTracker tracker(0);
      Counts a(tracker, "a", mode, grid, {true, 0, 64}, 4096, 10);
      Counts b(tracker, "b", mode, grid, {true, 0, 64}, 4096, 10);
      add(a, 0); add(b, 10, {GroupIdentity::DROP});
      EXPECT_THROW(reverse ? Counts::merge(&b, &a) : Counts::merge(&a, &b), std::runtime_error);
      EXPECT_THROW(add(a, -10, {GroupIdentity::DROP}), std::runtime_error);
      EXPECT_THROW(a.widen(-1, 10), std::runtime_error);
      EXPECT_EQ(1u, a.cellWindow()->size());
    }
  }
}

TEST(GrowingDistinctFacetTest, allocationAndConversionFailuresReleaseMemory) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  {
    RequestMemTracker tracker(2300 + sizeof(RangeFacetCellWindow<uint64_t**>));
    {
      Counts counts(tracker, "page failure", Mode::PAGED, grid, {true, 0, 1000000});
      add(counts, 0, {GroupIdentity::DROP});
      size_t before = tracker.bytes();
      EXPECT_THROW(add(counts, 0), ResourceExhaustedError);
      EXPECT_EQ(before, tracker.bytes());
      EXPECT_EQ(nullptr, rowAt(counts, 0));
    }
    EXPECT_EQ(0u, tracker.bytes());
  }
  {
    RequestMemTracker tracker(1024 + sizeof(RangeFacetCellWindow<uint64_t**>));
    {
      Counts counts(tracker, "relocation", Mode::PAGED, grid, {true, 0, 4096});
      add(counts, 0);
      auto** old = rowAt(counts, 0);
      size_t before = tracker.bytes();
      EXPECT_THROW(add(counts, 100), ResourceExhaustedError);
      EXPECT_EQ(before, tracker.bytes());
      EXPECT_EQ(old, rowAt(counts, 0));
      EXPECT_EQ(1u, counts.cellWindow()->size());
    }
    EXPECT_EQ(0u, tracker.bytes());
  }
  {
    RequestMemTracker tracker(16800 + sizeof(RangeFacetCellWindow<uint64_t**>));
    {
      Counts counts(tracker, "conversion", Mode::AUTO, grid, {true, 0, 4096}, 4096);
      for (int64_t i = 0; i < 31; i++) add(counts, i);
      add(counts, 31, {GroupIdentity::DROP});
      size_t before = tracker.bytes();
      auto** old = rowAt(counts, 0);
      EXPECT_THROW(add(counts, 31), ResourceExhaustedError);
      EXPECT_EQ(before, tracker.bytes());
      EXPECT_EQ(Mode::PAGED, counts.selectedMode());
      EXPECT_EQ(0u, counts.conversionCount());
      EXPECT_EQ(old, rowAt(counts, 0));
      EXPECT_EQ(1u, old[0][0]);
      EXPECT_LE(tracker.peak(), tracker.ceiling());
    }
    EXPECT_EQ(0u, tracker.bytes());
  }
}

TEST(GrowingDistinctFacetTest, pointerDirectoryRelocationOverlapCharged) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(0);
  {
    Counts counts(tracker, "overlap", Mode::PAGED, grid, {true, 0, 4096});
    add(counts, 0);
    size_t old = counts.cellWindow()->capacity() * sizeof(uint64_t**);
    size_t pages = tracker.bytes() - old;
    add(counts, 8, {GroupIdentity::DROP});
    EXPECT_EQ(pages + old + counts.cellWindow()->capacity() * sizeof(uint64_t**), tracker.peak());
    EXPECT_EQ(pages + counts.cellWindow()->capacity() * sizeof(uint64_t**), tracker.bytes());
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST(GrowingDistinctFacetTest, failedMergeRetainsOwningRowsAcrossLaterGrowth) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(2500 + 2 * sizeof(RangeFacetCellWindow<uint64_t**>));
  {
    Counts a(tracker, "destination", Mode::PAGED, grid, {true, 0, 4096}, 4096, 16);
    add(a, 0); add(a, 1, {GroupIdentity::DROP});
    {
      Counts b(tracker, "source", Mode::PAGED, grid, {true, 0, 4096}, 4096, 16);
      add(b, -2); add(b, -1);
      // Relocation and the first unioned row fit, the second row does not.
      EXPECT_THROW(Counts::merge(&a, &b), ResourceExhaustedError);
    }
    auto** partial = rowAt(a, -2);
    ASSERT_NE(nullptr, partial);
    add(a, 5, {GroupIdentity::DROP});
    EXPECT_EQ(partial, rowAt(a, -2));
  }
  EXPECT_EQ(0u, tracker.bytes());
}

class GrowingDistinctTupleTest : public LuxirTest {};
TEST_F(GrowingDistinctTupleTest, workerTupleIdsRemappedBothOrders) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "0", "key_s", "a", "key_i", INT64_MIN),
      flatdoc("id", "1", "key_s", "b", "key_i", INT64_MAX)}, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  CollapsePlan key(*reader, api::Collapse_::MissingPolicy::GROUP, false);
  key.fields.push_back({"key_s", api::GroupComponent_::Type::STRING, reader->getOrdMap("key_s"), reader->field("key_s")});
  key.fields.push_back({"key_i", api::GroupComponent_::Type::INT, {}, reader->field("key_i")});
  FacetGroupPlan plan(key);
  ASSERT_TRUE(plan.internTuples);
  CollapseKey first, second;
  first.components = {{true, 0}, {true, INT64_MIN}};
  second.components = {{true, 1}, {true, INT64_MAX}};
  auto grid = Grid::fixed(FieldType::Type::INT, 7);
  for (bool reverse : {false, true}) {
    RequestMemTracker tracker(0);
    Counts a(tracker, "tuple a", Mode::AUTO, grid, plan.dimension);
    Counts b(tracker, "tuple b", Mode::AUTO, grid, plan.dimension);
    a.collectValues([&](auto add, auto missing) {
      auto one = a.identity(plan, &first), two = a.identity(plan, &second);
      add(-10, one); add(20, two); missing(one);
    });
    b.collectValues([&](auto add, auto missing) {
      auto two = b.identity(plan, &second), one = b.identity(plan, &first);
      add(-10, two); add(20, one); missing(two);
    });
    auto* merged = reverse ? Counts::merge(&b, &a) : Counts::merge(&a, &b);
    merged->finish();
    EXPECT_EQ(2, merged->count(grid.floor(-10).key));
    EXPECT_EQ(2, merged->count(grid.floor(20).key));
    EXPECT_EQ(2, merged->missing());
  }
}
