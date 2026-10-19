// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <map>
#include <random>

#include "luxir/search/RangeFacetCountWindow.h"
#include "luxir/util/ApiError.h"
#include "test/RangeFacetTestUtils.h"
#include "luxir/util/NumericUtils.h"

using namespace luxir;
using luxir::test::epoch;
using luxir::test::zone;
namespace {
using Grid = RangeFacetGrid;
using Window = RangeFacetCountWindow;
using Oracle = std::map<int64_t, int64_t>;

// Independent counts and extent: map each observation to a canonical key,
// then fill gaps by walking the immutable grid. No window indices are used.
void check(const Grid& grid, const Window& window, const Oracle& occupied) {
  if (occupied.empty()) {
    EXPECT_TRUE(window.empty());
    EXPECT_TRUE(window.counts().empty());
    EXPECT_FALSE(window.occupiedBounds());
    return;
  }
  ASSERT_TRUE(window.occupiedBounds());
  EXPECT_EQ(occupied.begin()->first, window.occupiedBounds()->first);
  EXPECT_EQ(occupied.rbegin()->first, window.occupiedBounds()->second);
  auto counts = window.counts();
  size_t i = 0;
  auto cell = grid.floor(occupied.begin()->first);
  for (;;) {
    ASSERT_LT(i, counts.size());
    EXPECT_EQ(cell.key, window.lowerFence(i));
    auto found = occupied.find(cell.key);
    EXPECT_EQ(found == occupied.end() ? 0 : found->second, counts[i]);
    i++;
    if (cell.key == occupied.rbegin()->first) break;
    auto following = grid.next(cell);
    ASSERT_GT(following.key, cell.key);
    cell = following;
  }
  EXPECT_EQ(i, counts.size());
}

void randomized(const Grid& grid, std::vector<int64_t> values) {
  std::mt19937 random(57);
  for (bool ordered : {true, false}) {
    if (ordered) std::sort(values.begin(), values.end());
    else std::shuffle(values.begin(), values.end(), random);
    for (size_t splits : {size_t{1}, size_t{3}, size_t{8}}) {
      RequestMemTracker tracker(0);
      {
        std::vector<std::unique_ptr<Window>> windows;
        std::vector<Oracle> expected(splits);
        Oracle combined;
        for (size_t i = 0; i < splits; i++) windows.push_back(std::make_unique<Window>(grid, tracker, "oracle"));
        for (size_t i = 0; i < values.size(); i++) {
          int64_t weight = i % 5; // Includes raw occupancy with a zero measure.
          size_t split = ordered ? std::min(splits - 1, i * splits / values.size()) : i % splits;
          int64_t key = grid.floor(values[i]).key;
          expected[split][key] += weight;
          combined[key] += weight;
          windows[split]->add(values[i], weight);
        }
        for (size_t i = 0; i < splits; i++) check(grid, *windows[i], expected[i]);
        for (size_t i = 1; i < splits; i++) windows[0]->mergeFrom(*windows[i]);
        check(grid, *windows[0], combined);
      }
      EXPECT_EQ(0u, tracker.bytes());
    }
  }
}
}

TEST(RangeFacetCountWindowTest, emptySingleAndAlternatingEnds) {
  auto grid = Grid::fixed(FieldType::Type::INT, 7, 3);
  RequestMemTracker tracker(0);
  Window window(grid, tracker, "both ends", 100);
  Oracle expected;
  check(grid, window, expected);
  for (int i = 0; i < 40; i++) {
    int64_t value = i % 2 ? -i * 7 : i * 7;
    window.add(value, i % 3);
    expected[grid.floor(value).key] += i % 3;
    check(grid, window, expected);
    EXPECT_LE(window.capacity(), 100u);
  }
}

TEST(RangeFacetCountWindowTest, affineRandomizedMapOracle) {
  std::mt19937 random(123);
  std::vector<int64_t> values(1000);
  for (auto& value : values) value = (int64_t)(random() % 200000) - 100000;
  for (int64_t origin : {int64_t{0}, int64_t{17}}) {
    auto grid = Grid::fixed(FieldType::Type::INT, 13, origin);
    randomized(grid, values);
  }
}

TEST(RangeFacetCountWindowTest, nonAffineRandomizedMapOracle) {
  std::mt19937 random(123);
  for (auto [zone, date] : {std::pair{"America/New_York", "2024-03-10"},
                           std::pair{"America/New_York", "2024-11-03"},
                           std::pair{"Australia/Lord_Howe", "2024-10-05"},
                           std::pair{"Pacific/Apia", "2011-12-30"}}) {
    auto tz = resolveTimeZone(zone).value();
    for (auto grid : {Grid::fixed(FieldType::Type::DATE, 45 * 60000, 17 * 60000, tz),
                      Grid::calendar(CalendarUnit::DAY, 1, tz)}) {
      std::vector<int64_t> values(300);
      for (auto& value : values) value = epoch(date) + (int64_t)(random() % (5 * 86400000)) - 2 * 86400000;
      randomized(grid, values);
    }
  }
  for (auto type : {FieldType::Type::FLOAT, FieldType::Type::DOUBLE}) {
    auto grid = Grid::floating(type, 0.1, 0.07);
    std::vector<int64_t> values(300);
    for (auto& value : values) value = grid.encode(((int)(random() % 20000) - 10000) / 100.0);
    randomized(grid, values);
  }
}

TEST(RangeFacetCountWindowTest, disjointMergeBothOrdersAndFoldKeys) {
  for (bool reverse : {false, true}) {
    auto grid = Grid::fixed(FieldType::Type::DATE, 3600000, 17 * 60000,
                           resolveTimeZone("America/New_York").value());
    RequestMemTracker tracker(0);
    Window a(grid, tracker, "a"), b(grid, tracker, "b");
    Oracle expected;
    for (auto date : {"2024-11-02T01:20Z", "2024-11-03T05:20Z", "2024-11-03T06:20Z", "2024-11-04T08:20Z"}) {
      int64_t value = epoch(date);
      (value < epoch("2024-11-03T06:00Z") ? a : b).add(value);
      expected[grid.floor(value).key]++;
    }
    auto& merged = reverse ? b : a;
    merged.mergeFrom(reverse ? a : b);
    check(grid, merged, expected);
    // A merged contributor can be obtained again for another segment.
    for (auto date : {"2024-11-01T03:20Z", "2024-11-05T07:20Z"}) {
      int64_t value = epoch(date);
      merged.add(value);
      expected[grid.floor(value).key]++;
      check(grid, merged, expected);
    }
  }
}

TEST(RangeFacetCountWindowTest, affinityIncludesTransitionsInsideHeadroomAndLastCell) {
  auto grid = Grid::fixed(FieldType::Type::DATE, 3600000, 17 * 60000,
                         resolveTimeZone("America/New_York").value());
  RequestMemTracker tracker(0);
  Window window(grid, tracker, "transition");
  Oracle expected;
  for (auto date : {"2024-03-10T00:20Z", "2024-03-10T06:59Z", "2024-03-10T07:01Z",
                    "2024-03-10T07:18Z", "2024-03-10T10:20Z"}) {
    int64_t value = epoch(date);
    window.add(value);
    expected[grid.floor(value).key]++;
    check(grid, window, expected);
  }
  EXPECT_FALSE(window.affine());
}

TEST(RangeFacetCountWindowTest, capsUseActualExtentAtGrowthMergeAndWidening) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(0);
  for (bool reverse : {false, true}) {
    Window a(grid, tracker, "a", 10), b(grid, tracker, "b", 10);
    a.add(0); b.add(10);
    EXPECT_THROW(reverse ? b.mergeFrom(a) : a.mergeFrom(b), std::runtime_error);
    EXPECT_EQ(1u, a.counts().size());
    EXPECT_EQ(1u, b.counts().size());
  }
  Window window(grid, tracker, "growth", 10);
  window.add(0);
  EXPECT_GE(window.capacity(), 8u);
  window.add(9, 0);
  EXPECT_EQ(10u, window.counts().size());
  EXPECT_THROW(window.add(-1), std::runtime_error);
  EXPECT_THROW(window.widen(-1, 10), std::runtime_error);
  EXPECT_EQ(10u, window.counts().size());
  try { window.add(10); FAIL(); }
  catch (const std::runtime_error& error) {
    EXPECT_STREQ("range exceeds the 100000 bucket limit; use a larger gap", error.what());
  }
  Window exact(grid, tracker, "exclusive", 10);
  exact.widen(0, 10);
  EXPECT_EQ(10u, exact.counts().size());
  EXPECT_FALSE(exact.occupiedBounds());
  EXPECT_THROW(exact.widen(std::nullopt, 11), std::runtime_error);
}

TEST(RangeFacetCountWindowTest, alternatingEndsReachTheActualBucketLimit) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(0);
  Window window(grid, tracker, "full extent");
  for (int64_t i = 0; i < 100000; i++) window.add(i % 2 ? -(i + 1) / 2 : i / 2);
  ASSERT_EQ(100000u, window.counts().size());
  EXPECT_TRUE(std::ranges::all_of(window.counts(), [](int64_t count) { return count == 1; }));
  EXPECT_EQ(-50000, window.lowerFence(0));
  EXPECT_EQ(49999, window.lowerFence(99999));
  EXPECT_THROW(window.add(50000), std::runtime_error);
  Window other(grid, tracker, "outside");
  other.add(50000);
  EXPECT_THROW(window.mergeFrom(other), std::runtime_error);
}

TEST(RangeFacetCountWindowTest, encodedWideningEndIsExclusive) {
  RequestMemTracker tracker(0);
  for (auto type : {FieldType::Type::FLOAT, FieldType::Type::DOUBLE}) {
    auto grid = Grid::floating(type, 0.1, 0.07);
    auto first = grid.floor(grid.encode(-0.2));
    auto second = grid.next(first), end = grid.next(second);
    Window window(grid, tracker, "floating end");
    window.add(first.key, 0);
    window.widen(first.key, end.key);
    EXPECT_EQ(2u, window.counts().size());
    EXPECT_EQ(second.key, window.lowerFence(1));
    EXPECT_EQ(first.key, window.occupiedBounds()->second);
  }
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  Window window(grid, tracker, "minimum end");
  window.add(0);
  window.widen(std::nullopt, INT64_MIN);
  EXPECT_EQ(1u, window.counts().size());
}

TEST(RangeFacetCountWindowTest, nonAffineCapAndWideningDoNotCreateOccupancy) {
  auto grid = Grid::calendar(CalendarUnit::DAY, 1, resolveTimeZone("America/New_York").value());
  RequestMemTracker tracker(0);
  Window window(grid, tracker, "days", 4);
  window.add(epoch("2024-03-10T12:00Z"), 0);
  window.widen(epoch("2024-03-09T05:00Z"), epoch("2024-03-13T04:00Z"));
  EXPECT_EQ(4u, window.counts().size());
  ASSERT_TRUE(window.occupiedBounds());
  EXPECT_EQ(epoch("2024-03-10T05:00Z"), window.occupiedBounds()->first);
  EXPECT_EQ(window.occupiedBounds()->first, window.occupiedBounds()->second);
  EXPECT_THROW(window.add(epoch("2024-03-14T12:00Z")), std::runtime_error);
  Window other(grid, tracker, "other", 4);
  other.add(epoch("2024-03-15T12:00Z"));
  EXPECT_THROW(window.mergeFrom(other), std::runtime_error);
}

TEST(RangeFacetCountWindowTest, integerAndCivilRepresentationLimits) {
  RequestMemTracker tracker(0);
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  for (bool upper : {false, true}) {
    Window window(grid, tracker, "limits");
    Oracle expected;
    for (int64_t i = 0; i < 20; i++) {
      int64_t value = upper ? INT64_MAX - i : INT64_MIN + i;
      window.add(value);
      expected[value]++;
    }
    check(grid, window, expected);
  }
  // The physical span can cover every int64 value. Its unrepresentable
  // unsigned width uses cold lookup, without overflow or a false cap error.
  auto huge = Grid::fixed(FieldType::Type::INT, INT64_MAX, INT64_MIN);
  // Initial 2-to-3-cell growth needs 5 counters at peak. A needless later
  // 3-to-3-cell relocation would exceed this ceiling.
  RequestMemTracker hugeTracker(5 * sizeof(int64_t));
  Window wide(huge, hugeTracker, "wide");
  for (int64_t value : {INT64_MIN, int64_t{0}, INT64_MAX}) wide.add(value);
  size_t before = hugeTracker.peak();
  wide.add(0, 0);
  EXPECT_EQ(before, hugeTracker.peak());
  EXPECT_EQ(3u, wide.counts().size());
  EXPECT_EQ(1, wide.counts()[0]); EXPECT_EQ(1, wide.counts()[1]); EXPECT_EQ(1, wide.counts()[2]);
  auto dateGrid = Grid::fixed(FieldType::Type::DATE, 1);
  Window date(dateGrid, tracker, "affine date limits");
  date.add(INT64_MAX); date.add(INT64_MAX - 1);
  EXPECT_EQ(2u, date.counts().size());
  for (auto civil : {Grid::fixed(FieldType::Type::DATE, 60000),
                     Grid::calendar(CalendarUnit::DAY, 1, TimeZone::utc())}) {
    for (int64_t value : {datetime_detail::kMinEpochMs + 86400000,
                          datetime_detail::kMaxEpochMs - 2 * 86400000}) {
      Window window(civil, tracker, "civil");
      window.add(value); window.add(value + 86400000);
      EXPECT_FALSE(window.counts().empty());
    }
  }
  auto civil = Grid::fixed(FieldType::Type::DATE, 60000, 0,
                          resolveTimeZone("America/New_York").value());
  for (int64_t value : {datetime_detail::kMinEpochMs + 86400000,
                        datetime_detail::kMaxEpochMs - 2 * 86400000}) {
    Window window(civil, tracker, "civil");
    window.add(value); window.add(value + 60000);
    EXPECT_EQ(2u, window.counts().size());
  }
  Window invalid(civil, tracker, "outside civil");
  EXPECT_THROW(invalid.add(datetime_detail::kMinEpochMs - 1), std::runtime_error);
}

TEST(RangeFacetCountWindowTest, timeZoneCoverageCannotCauseNonAdvancingTraversal) {
  auto zone = resolveTimeZone("America/New_York").value();
  int64_t value = datetime_detail::kMaxEpochMs - 86400000;
  auto info = zone.ianaZone()->get_info(std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{value}});
  int64_t end = std::chrono::duration_cast<std::chrono::milliseconds>(info.end.time_since_epoch()).count();
  // Exercise the libstdc++ boundary when present; other tzdb implementations
  // may correctly cover the full civil range.
  if (end <= datetime_detail::kMaxEpochMs) {
    auto grid = Grid::fixed(FieldType::Type::DATE, 60000, 0, zone);
    RequestMemTracker tracker(0);
    Window window(grid, tracker, "tzdb boundary");
    window.add(end - 1); // Optional headroom must not reject this value.
    EXPECT_EQ(1u, window.counts().size());
    EXPECT_THROW(window.add(end), std::runtime_error);
    EXPECT_THROW(grid.affineGap(grid.floor(end - 1).key, end + 1), std::runtime_error);
  }
}

TEST(RangeFacetCountWindowTest, relocationOverlapChargedAndFailureReleasesMemory) {
  auto grid = Grid::fixed(FieldType::Type::INT, 1);
  {
    RequestMemTracker tracker(0);
    {
      Window window(grid, tracker, "relocation");
      window.add(0);
      size_t old = tracker.bytes();
      window.add(8);
      EXPECT_GE(tracker.peak(), old + tracker.bytes());
      EXPECT_EQ(window.capacity() * sizeof(int64_t), tracker.bytes());
    }
    EXPECT_EQ(0u, tracker.bytes());
  }
  RequestMemTracker tracker(8 * 16); // New 16-cell array fits alone, not beside old 8-cell array.
  {
    Window window(grid, tracker, "overlap");
    window.add(0);
    size_t old = tracker.bytes();
    EXPECT_THROW(window.add(8), ResourceExhaustedError);
    EXPECT_EQ(old, tracker.bytes());
    EXPECT_EQ(1u, window.counts().size());
    EXPECT_EQ(1, window.counts()[0]);
  }
  EXPECT_EQ(0u, tracker.bytes());
  auto nonAffine = Grid::calendar(CalendarUnit::MONTH, 1, TimeZone::utc());
  RequestMemTracker small(100);
  EXPECT_THROW({ Window window(nonAffine, small, "failed construction"); window.add(0); }, ResourceExhaustedError);
  EXPECT_EQ(0u, small.bytes());
}

TEST(RangeFacetCountWindowTest, memoryFailuresDuringNonAffineRelocationAreTransactional) {
  auto grid = Grid::fixed(FieldType::Type::DATE, 3600000, 17 * 60000,
                         resolveTimeZone("America/New_York").value());
  int failures = 0, successes = 0;
  for (size_t ceiling = 64; ceiling <= 4096; ceiling += 64) {
    RequestMemTracker tracker(ceiling);
    {
      Window window(grid, tracker, "transaction");
      window.add(epoch("2024-03-09T00:20Z"));
      size_t before = tracker.bytes();
      try { window.add(epoch("2024-03-10T07:01Z")); successes++; }
      catch (const ResourceExhaustedError&) {
        failures++;
        EXPECT_EQ(before, tracker.bytes());
        ASSERT_EQ(1u, window.counts().size());
        EXPECT_EQ(1, window.counts()[0]);
      }
    }
    EXPECT_EQ(0u, tracker.bytes());
  }
  EXPECT_GT(failures, 0);
  EXPECT_GT(successes, 0);
}

TEST(RangeFacetCountWindowTest, piecewiseGrowthAndMergeMatchMap) {
  std::mt19937_64 random(37);
  for (auto pair : {std::pair{"America/New_York", "2024-03-10T07:00Z"},
      std::pair{"America/New_York", "2024-11-03T06:00Z"},
      std::pair{"Australia/Lord_Howe", "2024-04-06T15:00Z"},
      std::pair{"Australia/Lord_Howe", "2024-10-05T15:30Z"},
      std::pair{"Pacific/Apia", "2011-12-30T10:00Z"}}) {
    auto zone = resolveTimeZone(pair.first).value();
    for (auto grid : {Grid::fixed(FieldType::DATE, 3600000, 17 * 60000, zone),
        Grid::fixed(FieldType::DATE, 86400000, 6 * 3600000, zone),
        Grid::calendar(CalendarUnit::DAY, 1, zone), Grid::calendar(CalendarUnit::WEEK, 1, zone)}) {
      std::vector<int64_t> values;
      int64_t center = epoch(pair.second);
      for (int i = 0; i < 128; i++) values.push_back(center - 15 * 86400000LL
          + (int64_t)(random() % (30 * 86400000LL)));
      randomized(grid, std::move(values));
    }
  }
}
