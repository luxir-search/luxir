// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <array>
#include <random>

#include "luxir/search/RangeFacetGrid.h"
#include "luxir/util/NumericUtils.h"
#include "test/RangeFacetTestUtils.h"

using namespace luxir;
using luxir::test::epoch;
using luxir::test::zone;
using luxir::test::oldCalendarFences;
namespace {
using Grid = RangeFacetGrid;
constexpr int64_t minute = 60000;
constexpr int64_t day = 86400000;

// Independent reference: scan UTC instants, testing the local lattice at each
// minute. At a forward jump scan the missing civil minutes too. All tested
// phases, offsets and transitions are minute-aligned; queries also probe the
// intervening milliseconds. No local resolver, floor or next is used here.
std::vector<int64_t> scanFences(TimeZone zone, int64_t center, int64_t gap, int64_t origin) {
  using namespace std::chrono;
  auto offset = [&](int64_t t) {
    if (zone.isUtc()) return int64_t{0};
    if (zone.isFixed()) return (int64_t)zone.offsetMinutes() * minute;
    return duration_cast<milliseconds>(zone.ianaZone()->get_info(
        sys_time<milliseconds>{milliseconds{t}}).offset).count();
  };
  std::vector<int64_t> result;
  int64_t begin = center - 3 * day, end = center + 3 * day;
  int64_t previous = offset(begin - minute);
  for (int64_t t = begin; t <= end; t += minute) {
    int64_t current = offset(t);
    bool fence = (t + current - origin) % gap == 0;
    for (int64_t missing = t + previous; missing < t + current && !fence; missing += minute) {
      fence = (missing - origin) % gap == 0;
    }
    if (fence) result.push_back(t);
    previous = current;
  }
  return result;
}

} // namespace

TEST(RangeFacetGridTest, instantScanningOracle) {
  struct Case { std::string_view zone; std::string_view center; };
  for (auto c : std::array{
      Case{"America/New_York", "2024-03-10T07:00Z"},
      Case{"America/New_York", "2024-11-03T06:00Z"},
      Case{"America/Denver", "2024-03-10T09:00Z"},
      Case{"America/Denver", "2024-11-03T08:00Z"},
      Case{"Australia/Lord_Howe", "2024-04-06T15:00Z"},
      Case{"Australia/Lord_Howe", "2024-10-05T15:30Z"},
      Case{"Pacific/Apia", "2011-12-30T11:00Z"},
      Case{"America/New_York", "1969-10-26T06:00Z"},
      Case{"UTC", "1969-01-05T00:00Z"}, Case{"+05:30", "1969-01-05T00:00Z"}}) {
    for (int64_t gap : {15 * minute, 45 * minute, 60 * minute, 120 * minute, day}) {
      for (int64_t origin : {int64_t{0}, 5 * minute, 17 * minute, 360 * minute}) {
        SCOPED_TRACE(std::string(c.zone) + " " + std::string(c.center) + " gap="
                     + std::to_string(gap) + " origin=" + std::to_string(origin));
        int64_t center = epoch(c.center);
        auto reference = scanFences(zone(c.zone), center, gap, origin);
        auto grid = Grid::fixed(FieldType::Type::DATE, gap, origin, zone(c.zone));
        for (size_t i = 1; i + 1 < reference.size(); i++) {
          int64_t lower = reference[i], upper = reference[i + 1];
          if (lower < center - day || upper > center + day) continue;
          for (int64_t v : {lower, lower + 1, lower + (upper - lower) / 2, upper - 1}) {
            auto cell = grid.floor(v);
            ASSERT_EQ(lower, cell.key) << "value=" << v;
            ASSERT_EQ(upper, grid.next(cell).key) << "value=" << v;
          }
        }
        int64_t lo = reference[1], hi = reference[reference.size() - 2];
        auto built = grid.fences(lo, hi, true);
        ASSERT_EQ(std::vector<int64_t>(reference.begin() + 1, reference.end() - 1), built.values);
        if (built.affineGap) {
          EXPECT_EQ(gap, built.affineGap);
          for (size_t i = 1; i < built.values.size(); i++) EXPECT_EQ(gap, built.values[i] - built.values[i - 1]);
        }
      }
    }
  }
}

TEST(RangeFacetGridTest, affinityRequiresTransitionFenceOnGrid) {
  int64_t lo = epoch("2024-03-10T05:00Z"), hi = epoch("2024-03-10T10:00Z");
  auto ny = zone("America/New_York");
  auto hourly = Grid::fixed(FieldType::Type::DATE, 60 * minute, 0, ny);
  EXPECT_EQ(60 * minute, hourly.affineGap(lo, hi));
  // Offsets agree modulo ten minutes, but the clamped transition does not.
  auto shifted = Grid::fixed(FieldType::Type::DATE, 10 * minute, 5 * minute, ny);
  EXPECT_FALSE(shifted.affineGap(lo + 5 * minute, hi));
  EXPECT_EQ(epoch("2024-03-10T07:00Z"), shifted.floor(epoch("2024-03-10T07:02Z")).key);
  EXPECT_EQ(epoch("2024-03-10T07:05Z"), shifted.next(shifted.floor(epoch("2024-03-10T07:02Z"))).key);
  // The same phase is affine across a fold, where no gap fences are inserted.
  lo = epoch("2024-11-03T04:05Z"); hi = epoch("2024-11-03T10:00Z");
  EXPECT_EQ(10 * minute, shifted.affineGap(lo, hi));
  auto halfHour = Grid::fixed(FieldType::Type::DATE, 60 * minute, 0, zone("Australia/Lord_Howe"));
  EXPECT_FALSE(halfHour.affineGap(epoch("2024-10-05T13:30Z"), epoch("2024-10-06T13:00Z")));
}

TEST(RangeFacetGridTest, epochAnchoredCalendar) {
  struct Case { CalendarUnit unit; int n; std::string_view value, lower, upper; };
  for (auto c : std::array{
      Case{CalendarUnit::MONTH, 2, "2024-02-15", "2024-01-01", "2024-03-01"},
      Case{CalendarUnit::MONTH, 2, "1969-12-31", "1969-11-01", "1970-01-01"},
      Case{CalendarUnit::MONTH, 3, "1969-12-31", "1969-10-01", "1970-01-01"},
      Case{CalendarUnit::MONTH, 3, "2024-05-12", "2024-04-01", "2024-07-01"},
      Case{CalendarUnit::WEEK, 2, "1970-01-05", "1969-12-29", "1970-01-12"},
      Case{CalendarUnit::WEEK, 2, "1969-12-28", "1969-12-15", "1969-12-29"},
      Case{CalendarUnit::YEAR, 10, "2024-06-01", "2020-01-01", "2030-01-01"},
      Case{CalendarUnit::YEAR, 10, "1969-12-31", "1960-01-01", "1970-01-01"},
      Case{CalendarUnit::QUARTER, 1, "2024-02-15", "2024-01-01", "2024-04-01"},
      Case{CalendarUnit::QUARTER, 2, "2024-05-15", "2024-01-01", "2024-07-01"},
      Case{CalendarUnit::DAY, 3, "1969-12-31", "1969-12-29", "1970-01-01"}}) {
    SCOPED_TRACE(std::string(c.value) + " n=" + std::to_string(c.n));
    for (auto name : {"UTC", "+05:30", "America/New_York", "Australia/Lord_Howe"}) {
      SCOPED_TRACE(name);
      auto z = zone(name);
      auto local = [&](std::string_view text) { return parseDateRange(text, 0, z)->lo; };
      auto grid = Grid::calendar(c.unit, c.n, z);
      auto cell = grid.floor(local(c.value));
      EXPECT_EQ(local(c.lower), cell.key);
      EXPECT_EQ(local(c.upper), grid.next(cell).key);
    }
  }
}

TEST(RangeFacetGridTest, explicitFingerprints) {
  // A separate transcription of the old affine and floating builders.
  for (auto z : {"UTC", "+05:30", "America/New_York"}) {
    for (int64_t start : {epoch("2024-01-15T05:17Z"), epoch("1969-01-15T05:17Z")}) {
      int64_t end = start + 3 * day + 1, gap = 45 * minute;
      auto grid = Grid::fixed(FieldType::Type::DATE, gap, civilFromInstant(start, zone(z))->millis, zone(z));
      std::vector<int64_t> old;
      for (int64_t value = start; value < end; value += gap) old.push_back(value);
      old.push_back(end);
      EXPECT_EQ(old, grid.fences(start, end, true).values);
      EXPECT_EQ(gap, grid.fences(start, end, true).affineGap);
    }
  }
  auto integer = Grid::fixed(FieldType::Type::INT, INT64_MAX, INT64_MIN);
  EXPECT_EQ((std::vector<int64_t>{INT64_MIN, -1, INT64_MAX - 1, INT64_MAX}),
            integer.fences(INT64_MIN, INT64_MAX, true).values);
  int64_t start = epoch("2024-01-01"), end = epoch("2025-01-01");
  auto ny = zone("America/New_York");
  auto hugeDate = Grid::fixed(FieldType::Type::DATE, INT64_MAX, civilFromInstant(start, ny)->millis, ny);
  EXPECT_EQ((std::vector<int64_t>{start, end}), hugeDate.fences(start, end, true).values);
  struct FP { double start, end, gap; };
  for (auto type : {FieldType::Type::FLOAT, FieldType::Type::DOUBLE}) {
    for (auto c : std::array{FP{0.3, 1.05, 0.1}, FP{-1.86, 1.9, 0.31},
                           FP{1e12, 1e12 + 3e6, 1e6}, FP{0, 1, 1e300}}) {
      auto grid = Grid::floating(type, c.gap, c.start);
      std::vector<int64_t> old;
      for (int i = 0; ; i++) {
        double value = std::min(c.start + i * c.gap, c.end);
        old.push_back(grid.encode(value));
        if (value == c.end) break;
      }
      EXPECT_EQ(old, grid.fences(grid.encode(c.start), grid.encode(c.end), true).values);
    }
  }
  // Preserve signed zero after FLOAT rounding and clip before an overflowing
  // nominal next fence, as the old EXPLICIT builder did.
  auto tiny = Grid::floating(FieldType::Type::FLOAT, 2e-46, -1e-46);
  EXPECT_EQ((std::vector<int64_t>{floatToSortableInt32(-0.0f), floatToSortableInt32(0.0f)}),
            tiny.fences(tiny.encode(-1e-46), tiny.encode(1e-46), true).values);
  auto huge = Grid::floating(FieldType::Type::DOUBLE, 1e308, 1e308);
  EXPECT_EQ((std::vector<int64_t>{doubleToSortableInt64(1e308), doubleToSortableInt64(1.5e308)}),
            huge.fences(huge.encode(1e308), huge.encode(1.5e308), true).values);
  auto calendar = Grid::calendar(CalendarUnit::MONTH, 1, TimeZone::utc(), epoch("2024-01-31"));
  EXPECT_EQ((std::vector<int64_t>{epoch("2024-01-31"), epoch("2024-02-29"), epoch("2024-03-31"), epoch("2024-04-01")}),
      calendar.fences(epoch("2024-01-31"), epoch("2024-04-01"), true).values);
}

TEST(RangeFacetGridTest, floatingEncodedCoverageAndGlobalIndices) {
  struct Case { double origin, gap; int64_t first; };
  for (auto type : {FieldType::Type::FLOAT, FieldType::Type::DOUBLE}) {
    for (auto c : std::array{Case{0, .1, -25}, Case{.17, .31, -12}, Case{1e12, 1e6, -4},
                           Case{-1e30, 1e25, -3}, Case{0, .1, 1000000},
                           Case{0, type == FieldType::Type::FLOAT ? 1e25 : .1,
                                type == FieldType::Type::FLOAT ? 100000 : 1000000000000LL}}) {
      auto grid = Grid::floating(type, c.gap, c.origin);
      for (int64_t k = c.first; k < c.first + 12; k++) {
        int64_t key = grid.encode(c.origin + (double)k * c.gap);
        auto cell = grid.floor(key);
        ASSERT_EQ(key, cell.key);
        EXPECT_EQ(k, cell.index);
        auto following = grid.next(cell);
        EXPECT_EQ(grid.encode(c.origin + (double)(k + 1) * c.gap), following.key);
        EXPECT_EQ(key, grid.floor(following.key - 1).key);
        auto span = grid.fences(key, following.key - 1, false);
        EXPECT_EQ((std::vector<int64_t>{key, following.key}), span.values);
      }
    }
  }
}

TEST(RangeFacetGridTest, explicitCalendarFromStartFingerprints) {
  for (auto z : {"UTC", "+05:30", "America/New_York", "America/Havana", "Pacific/Apia"}) {
    for (auto text : {"1969-01-31T05:17Z", "2024-01-31T05:17Z", "2024-03-09T07:30Z",
                      "2024-11-03T06:30Z", "2011-12-29T10:00Z", "2011-12-31T11:00Z"}) {
      for (auto unit : {CalendarUnit::DAY, CalendarUnit::WEEK, CalendarUnit::MONTH,
                        CalendarUnit::QUARTER, CalendarUnit::YEAR}) {
        SCOPED_TRACE(std::string(z) + " " + text + " unit=" + std::to_string((int)unit));
        int64_t start = epoch(text), end = start + 400 * day;
        for (int64_t n : {1, 2, 3}) {
          SCOPED_TRACE("n=" + std::to_string(n));
          auto grid = Grid::calendar(unit, n, zone(z), start);
          auto built = grid.fences(start, end, true).values;
          EXPECT_EQ(oldCalendarFences(start, end, unit, zone(z), n), built);
          // The call-local construction cache must preserve point operations.
          for (size_t i = 0; i + 2 < built.size(); i++) {
            auto cell = grid.floor(built[i]);
            ASSERT_EQ(built[i], cell.key);
            ASSERT_EQ(built[i + 1], grid.next(cell).key);
          }
        }
      }
    }
  }
}

TEST(RangeFacetGridTest, calendarFoldPreferenceBelongsToSpec) {
  auto havana = zone("America/Havana");
  auto epochGrid = Grid::calendar(CalendarUnit::MONTH, 1, havana);
  auto early = epochGrid.floor(epoch("2015-11-01T04:30Z"));
  EXPECT_EQ(epoch("2015-11-01T04:00Z"), early.key);
  EXPECT_EQ(epoch("2015-12-01T05:00Z"), epochGrid.next(early).key);
  EXPECT_EQ(early.key, epochGrid.floor(epoch("2015-11-01T05:30Z")).key);
  auto fromWinter = Grid::calendar(CalendarUnit::MONTH, 1, havana, epoch("2015-01-01T05:00Z"));
  EXPECT_EQ(epoch("2015-11-01T05:00Z"), fromWinter.next(fromWinter.floor(epoch("2015-10-15"))).key);
  // EXPLICIT retains its own start offset, including a summer preference.
  auto fromSummer = Grid::calendar(CalendarUnit::MONTH, 1, havana, epoch("2015-07-01T04:00Z"));
  EXPECT_EQ(epoch("2015-11-01T04:00Z"), fromSummer.floor(epoch("2015-11-01T04:30Z")).key);
}

TEST(RangeFacetGridTest, bucketCapAdviceFollowsBounds) {
  auto check = [](const Grid& grid, int64_t lo, int64_t hi) {
    for (bool clip : {false, true}) {
      try {
        grid.fences(lo, hi, clip, 1);
        FAIL() << "expected bucket cap error";
      } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string("range exceeds the 1 bucket limit")
                      + (clip ? "" : "; use a larger gap"), error.what());
      }
    }
  };
  check(Grid::fixed(FieldType::Type::INT, 1), 0, 2);
  auto floating = Grid::floating(FieldType::Type::DOUBLE, 1);
  check(floating, floating.encode(0), floating.encode(2));
  auto ny = zone("America/New_York");
  check(Grid::calendar(CalendarUnit::MONTH, 1, ny), epoch("2024-01-01T05:00Z"), epoch("2024-03-01T05:00Z"));
  check(Grid::fixed(FieldType::Type::DATE, 60 * minute, 17 * minute, ny),
        epoch("2024-03-10T05:17Z"), epoch("2024-03-10T08:17Z"));
}

TEST(RangeFacetGridTest, calendarSkippedIntervalCacheMatchesDirectLookup) {
  struct Case { std::string_view zone, center; };
  for (auto c : std::array{
      Case{"America/New_York", "2024-03-10"},
      Case{"America/New_York", "1969-10-26"},
      Case{"America/Havana", "2015-11-01"},
      Case{"Australia/Lord_Howe", "2024-10-06"},
      Case{"Pacific/Apia", "2011-12-30"}}) {
    SCOPED_TRACE(std::string(c.zone));
    auto z = zone(c.zone);
    for (int direction : {1, -1}) {
      datetime_detail::LocalTimeResolver resolver(z, std::nullopt);
      for (int step = -48; step <= 48; step++) {
        int64_t civil = epoch(c.center) + direction * step * 60 * minute;
        ASSERT_TRUE(resolver.resolve(civil));
        for (int64_t width : {int64_t{1}, 60 * minute, day, 7 * day, 40 * day}) {
          EXPECT_EQ(civilIntervalSkipped(civil, civil + width, z),
                    resolver.intervalSkipped(civil, civil + width))
              << "civil=" << civil << " width=" << width;
        }
      }
    }
  }
}

TEST(RangeFacetGridTest, piecewiseRunsMatchFenceSearch) {
  struct Case { std::string_view name, center; };
  std::mt19937_64 random(37);
  for (auto c : {Case{"America/New_York", "2024-03-10T07:00Z"},
      Case{"America/New_York", "2024-11-03T06:00Z"},
      Case{"America/New_York", "1969-10-26T06:00Z"},
      Case{"Australia/Lord_Howe", "2024-04-06T15:00Z"},
      Case{"Australia/Lord_Howe", "2024-10-05T15:30Z"},
      Case{"Pacific/Apia", "2011-12-30T10:00Z"},
      Case{"+05:30", "1969-03-10T07:00Z"}, Case{"UTC", "1969-03-10T07:00Z"}}) {
    auto z = zone(c.name);
    int64_t center = epoch(c.center);
    for (bool clip : {false, true}) for (int kind = 0; kind < 7; kind++) {
      for (int64_t phase : {int64_t{0}, 5 * minute, 17 * minute, 360 * minute}) {
        SCOPED_TRACE(std::string(c.name) + " kind=" + std::to_string(kind)
            + " phase=" + std::to_string(phase) + " clip=" + std::to_string(clip));
        int64_t lo = center - 15 * day + (int64_t)(random() % day);
        int64_t hi = center + 15 * day - (int64_t)(random() % day);
        auto grid = kind >= 5 ? Grid::calendar(kind == 5 ? CalendarUnit::DAY : CalendarUnit::WEEK,
            1, z, clip ? std::optional<int64_t>(lo) : std::nullopt)
            : Grid::fixed(FieldType::DATE, std::array<int64_t, 5>{15 * minute, 45 * minute,
                60 * minute, 120 * minute, day}[kind],
                clip ? civilFromInstant(lo, z)->millis : phase, z);
        auto fences = grid.fences(lo, hi, clip).values;
        auto runs = grid.runs(fences);
        ASSERT_FALSE(runs.empty());
        EXPECT_EQ(fences.front(), runs.front().first);
        EXPECT_EQ(fences.back(), (int64_t)((uint64_t)runs.back().first + runs.back().width));
        std::vector<int64_t> values;
        for (size_t i = 0; i + 1 < fences.size(); i++) {
          values.push_back(fences[i]);
          values.push_back(fences[i + 1] - 1);
        }
        for (int i = 0; i < 128; i++) values.push_back(fences.front()
            + (int64_t)(random() % ((uint64_t)fences.back() - (uint64_t)fences.front())));
        for (bool ordered : {true, false}) {
          if (ordered) std::sort(values.begin(), values.end());
          else std::shuffle(values.begin(), values.end(), random);
          RangeFacetRunCursor cursor;
          for (int64_t value : values) {
            size_t expected = (size_t)(std::upper_bound(fences.begin(), fences.end(), value) - fences.begin() - 1);
            ASSERT_EQ(expected, cursor.bucketOf(value, runs, fences)) << value;
          }
        }
      }
    }
  }
  // Month/quarter/year and floating grids retain their fence search.
  for (auto grid : {Grid::calendar(CalendarUnit::MONTH, 1, TimeZone::utc()),
                    Grid::floating(FieldType::DOUBLE, 1)}) {
    auto fences = grid.fences(0, 100, true).values;
    auto runs = grid.runs(fences);
    EXPECT_TRUE(runs.empty());
    RangeFacetRunCursor cursor;
    EXPECT_EQ(0u, cursor.bucketOf(fences.front(), runs, fences));
  }
}
