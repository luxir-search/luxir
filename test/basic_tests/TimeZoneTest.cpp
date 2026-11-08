// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/search/RangeFacetGrid.h"
#include "test/RangeFacetTestUtils.h"
#include "test/ZoneinfoFixture.h"

using namespace luxir;
using namespace luxir::test;

namespace {
using Grid = RangeFacetGrid;
constexpr int64_t hour = 3600000;
constexpr int64_t day = 24 * hour;

int64_t unixSeconds(std::string_view text) { return epoch(text) / 1000; }

// The interval and local-span invariants that traversals advance by.
void expectSpans(const IanaZone& zone, int64_t instant) {
  auto interval = zone.intervalAt(instant);  // throws if it misses instant
  EXPECT_EQ(interval.offset, zone.offsetAt(interval.begin));
  EXPECT_EQ(interval.offset, zone.offsetAt(interval.end - 1));
  if (interval.end <= datetime_detail::kMaxEpochMs) {
    EXPECT_NE(interval.offset, zone.offsetAt(interval.end));
  }
  int64_t civil = instant + interval.offset.count() * 1000;
  auto local = zone.localAt(civil);  // throws if its span misses civil
  for (int64_t edge : {local.civilBegin, local.civilEnd - 1}) {
    auto at = zone.localAt(edge);
    EXPECT_EQ(local.kind, at.kind) << "edge=" << edge;
    EXPECT_EQ(local.before, at.before) << "edge=" << edge;
  }
}

} // namespace

TEST(TimeZoneTest, registryTakesNamesFromTzdataZi) {
  ZoneinfoFixture fixture;
  fixture.zone("Test/Zone", {{-7 * 3600, false, "MST"}}, {}, "MST7");
  fixture.zone("Test/Tab", {{3600, false, "TAB"}}, {}, "TAB-1");
  fixture.record("Z Test/Zone -7 - MST");
  fixture.record("Z\tTest/Tab\t1\t-\tTAB");
  fixture.record("Z Test/Missing 0 - X");
  fixture.record("L Test/Zone Test/Alias");
  std::string previous = "Test/Alias";
  for (int i = 0; i < 12; i++) {
    fixture.record("L " + previous + " Test/Chain" + std::to_string(i));
    previous = "Test/Chain" + std::to_string(i);
  }
  fixture.record("L Test/CycleA Test/CycleB");
  fixture.record("L Test/CycleB Test/CycleA");
  fixture.record("L Test/Missing Test/Dangling");
  auto registry = fixture.load();
  EXPECT_EQ("", registry.error);
  EXPECT_EQ("2099z", registry.version);
  EXPECT_EQ(2u, registry.zoneCount());
  const IanaZone* zone = registry.find("Test/Zone");
  ASSERT_NE(nullptr, zone);
  EXPECT_EQ("Test/Zone", zone->name());
  EXPECT_EQ(-7 * 3600, zone->offsetAt(0).count());
  EXPECT_EQ(zone, registry.find("Test/Alias"));  // links need no TZif file of their own
  EXPECT_EQ(zone, registry.find("Test/Chain11"));
  EXPECT_NE(nullptr, registry.find("Test/Tab"));
  for (std::string_view name : {"Test/Missing", "Test/Dangling", "Test/CycleA", "test/zone",
                                "Test", "tzdata.zi"}) {
    EXPECT_EQ(nullptr, registry.find(name)) << name;
  }
  auto relative = datetime_detail::ZoneRegistry::load(
      std::filesystem::relative(fixture.path()).string());
  EXPECT_NE(nullptr, relative.find("Test/Alias"));
  std::filesystem::remove(fixture.path() / "tzdata.zi");
  EXPECT_NE("", datetime_detail::ZoneRegistry::load(fixture.path().string()).error);
}

// Transitions exactly 400 years apart are not a recurring rule: this offset
// stays put after 2400, and traversal there still advances.
TEST(TimeZoneTest, finiteHistoryEndsAtItsLastTransition) {
  ZoneinfoFixture fixture;
  fixture.zone("Test/Finite", {{0, false, "OLD"}, {3600, false, "MID"}, {0, false, "END"}},
               {{unixSeconds("2000-01-01"), 1}, {unixSeconds("2400-01-01"), 2}}, "END0");
  fixture.record("Z Test/Finite 0 - END");
  auto registry = fixture.load();
  const IanaZone& zone = *registry.find("Test/Finite");
  auto interval = zone.intervalAt(epoch("2800-01-01"));
  EXPECT_EQ(epoch("2400-01-01"), interval.begin);
  EXPECT_EQ(datetime_detail::kMaxEpochMs + 1, interval.end);
  EXPECT_EQ(0, interval.offset.count());
  for (auto t : {"1999-12-31T23:59:59.999Z", "2000-01-01", "2399-12-31T23:59:59.999Z",
                 "2400-01-01", "2800-01-01T00:30Z"}) {
    SCOPED_TRACE(t);
    expectSpans(zone, epoch(t));
  }
  auto grid = Grid::fixed(FieldType::Type::DATE, hour, 0, TimeZone::iana(&zone));
  EXPECT_EQ(epoch("2800-01-01T00:00Z"), grid.floor(epoch("2800-01-01T00:30Z")).key);
}

// A recurring footer rule applies through the last supported year, to offsets
// and to transition traversal alike.
TEST(TimeZoneTest, recurringRulesCoverTheCivilRange) {
  ZoneinfoFixture fixture;
  fixture.zone("Test/Eastern", {{-5 * 3600, false, "EST"}, {-4 * 3600, true, "EDT"}},
               {{unixSeconds("2007-03-11T07:00Z"), 1}, {unixSeconds("2007-11-04T06:00Z"), 0}},
               "EST5EDT,M3.2.0,M11.1.0");
  fixture.record("Z Test/Eastern -5 - EST");
  auto registry = fixture.load();
  const IanaZone& zone = *registry.find("Test/Eastern");
  EXPECT_EQ(-4 * 3600, zone.offsetAt(epoch("32767-07-01")).count());
  EXPECT_EQ(-5 * 3600, zone.offsetAt(epoch("32767-12-01")).count());
  for (auto t : {"2408-06-01", "2408-12-01", "2409-03-01", "2500-07-01",
                 "9999-03-14T06:59:59.999Z", "9999-03-14T07:00Z", "32767-06-01"}) {
    SCOPED_TRACE(t);
    expectSpans(zone, epoch(t));
  }
  auto gap = zone.localAt(epoch("9999-03-14T02:30Z"));  // civil 02:30
  EXPECT_EQ(LocalOffsets::Kind::SKIPPED, gap.kind);
  EXPECT_EQ(epoch("9999-03-14T02:00Z"), gap.civilBegin);
  EXPECT_EQ(epoch("9999-03-14T03:00Z"), gap.civilEnd);
  auto grid = Grid::fixed(FieldType::Type::DATE, day, 0, TimeZone::iana(&zone));
  for (std::string_view first : {"2500-03-01", "9999-03-01", "32767-03-01"}) {
    int64_t start = grid.floor(epoch(first)).key;
    EXPECT_FALSE(grid.affineGap(start, start + 40 * day)) << first;
  }
}

TEST(TimeZoneTest, installedZonesKeepSpanInvariants) {
  const auto& registry = datetime_detail::zoneRegistry();
  ASSERT_EQ("", registry.error);
  for (size_t i = 0; i < registry.zoneCount() && !HasFailure(); i++) {
    const IanaZone& zone = registry.zone(i);
    SCOPED_TRACE(std::string(zone.name()));
    for (int64_t t = epoch("1850-01-01"); t < epoch("2100-01-01") && !HasFailure();) {
      int64_t transition = zone.intervalAt(t).end;
      if (transition > datetime_detail::kMaxEpochMs) break;
      expectSpans(zone, transition - 1);
      expectSpans(zone, transition);
      t = transition;
    }
  }
}
