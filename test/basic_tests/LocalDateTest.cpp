// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// LOCAL_DATE: calendar dates stored like DATE (epoch millis of UTC midnight)
// whose ingest, query literals and calendar facets ignore time_zone.

#include <gtest/gtest.h>
#include <algorithm>
#include <functional>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/util/DateTime.h"

using namespace luxir;
using namespace luxir::test;
using FieldClass = api::FieldDef::FieldClass;
using Status = api::UpdateResponse_::Status;

namespace {

int64_t day(std::string_view text) { return *parseDateToEpochMillis(text); }

std::vector<std::pair<int64_t, int64_t>> bucketBounds(const api::FacetResult& result) {
  std::vector<std::pair<int64_t, int64_t>> out;
  for (const auto& b : std::get<api::ArrArrInt>(result.bucket_ids->kind).v) {
    out.emplace_back(b.v[0], b.v[1]);
  }
  return out;
}

std::vector<int64_t> bucketCounts(const api::FacetResult& result) {
  return {result.counts.begin(), result.counts.end()};
}

const api::FacetResult& facetResult(const LocalReq& req, std::string_view name) {
  return *req.responses[0]->proto.ops.at(name)->facetResult();
}

} // namespace

class LocalDateTest : public LuxirTest {
protected:
  static int64_t count(std::string_view zone, std::function<void(OpCursor&)> setQuery) {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").timeZone(zone).topDocs("q");
    setQuery(cur);
    cur.withStats();
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getMatchCount();
  }
};

// Ingest takes whole days only; a time of day fails the doc, not truncates.
TEST_F(LocalDateTest, ingestAcceptsOnlyCalendarDates) {
  CollectionHelper helper;
  std::vector<Doc> docs = {
    flatdoc("id", "iso", "birthday_ld", "2024-06-25"),
    flatdoc("id", "month", "birthday_ld", "2024-06"),
    flatdoc("id", "millis", "birthday_ld", day("2024-06-25")),
    flatdoc("id", "midnight", "birthday_ld", "2024-06-25T00:00:00Z"),
    flatdoc("id", "offsetMidnight", "birthday_ld", "2024-06-25T05:30:00+05:30"),
    flatdoc("id", "rounded", "birthday_ld", "2024-06-25T13:00:00Z||/d"),
    flatdoc("id", "multi", "dates_lds", std::vector<std::string>{"2024-01-01", "2024-02-29"}),
    flatdoc("id", "noon", "birthday_ld", "2024-06-25T12:00"),
    flatdoc("id", "zoned", "birthday_ld", "2024-06-25T00:00:00-05:00"),
    flatdoc("id", "offByOne", "birthday_ld", day("2024-06-25") + 1),
    flatdoc("id", "now", "birthday_ld", "NOW"),
    flatdoc("id", "multiBad", "dates_lds", std::vector<std::string>{"2024-01-01", "2024-01-01T01:00Z"}),
  };
  auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_EQ(Status::PARTIAL, result.status);
  std::vector<std::string> failed;
  for (const auto& error : result.errors) {
    failed.emplace_back(error.id);
    EXPECT_EQ("invalid_value", error.code);
  }
  std::sort(failed.begin(), failed.end());
  EXPECT_EQ((std::vector<std::string>{"multiBad", "noon", "now", "offByOne", "zoned"}), failed);
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("is not a calendar date"));

  // Output is DATE's: epoch millis of UTC midnight.
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("id", "month").fields({"birthday_ld"});
  req->execute();
  EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("birthday_ld", day("2024-06-01"))));
}

// The same literal finds the same date under every request zone, while a DATE
// midnight shifts out of it.
TEST_F(LocalDateTest, queryLiteralsIgnoreTimeZone) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "a", "birthday_ld", "2024-06-25", "when_dt", "2024-06-25"),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "b", "birthday_ld", "2024-06-26"), UpdateMessage::COMMIT);

  for (std::string_view zone : {"UTC", "America/New_York", "Pacific/Kiritimati", "-12:00"}) {
    SCOPED_TRACE(zone);
    EXPECT_EQ(1, count(zone, [](OpCursor& c) { c.matchQuery("birthday_ld", "2024-06-25"); }));
    EXPECT_EQ(2, count(zone, [](OpCursor& c) { c.matchQuery("birthday_ld", "2024-06"); }));
    EXPECT_EQ(1, count(zone, [](OpCursor& c) {
      c.exprQuery("birthday_ld:[2024-06-26 TO 2024-06-30]");
    }));
    EXPECT_EQ(1, count(zone, [](OpCursor& c) {
      c.exprQuery("birthday_ld:{2024-06-25 TO 2024-06-27}");
    }));
    EXPECT_EQ(1, count(zone, [](OpCursor& c) {
      c.simpleQuery("birthday_ld:2024-06-26", {"id"});
    }));
    // Date math rounds in UTC, not in the request zone.
    EXPECT_EQ(1, count(zone, [](OpCursor& c) {
      c.exprQuery("birthday_ld:2024-06-26T05:00:00Z||/d");
    }));
    // A time of day is a valid query window that holds no calendar date.
    EXPECT_EQ(0, count(zone, [](OpCursor& c) { c.matchQuery("birthday_ld", "2024-06-25T12:00"); }));
  }
  EXPECT_EQ(0, count("America/New_York", [](OpCursor& c) { c.matchQuery("when_dt", "2024-06-25"); }));
}

// Calendar buckets follow the UTC calendar whatever time_zone says, and DAY
// buckets are fixed width; the same midnights in a DATE field shift a day.
TEST_F(LocalDateTest, calendarFacetsIgnoreTimeZone) {
  CollectionHelper helper;
  const std::array dates = {"2024-03-09", "2024-03-10", "2024-03-10", "2024-03-11"};
  for (size_t i = 0; i < dates.size(); i++) {
    helper.index(flatdoc("id", std::to_string(i), "day_ld", dates[i], "when_dt", dates[i]),
                 i + 1 == dates.size() ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  auto facet = [&](std::string_view field, std::string_view requestZone,
                   std::string_view facetZone, api::CalendarGap_::Unit unit) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main").timeZone(requestZone);
    req->rangeFacet("f", field).calendarRange("2024-03-09", "2024-03-12", 1, unit, facetZone);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return std::pair{bucketBounds(facetResult(*req, "f")), bucketCounts(facetResult(*req, "f"))};
  };

  const std::vector<std::pair<int64_t, int64_t>> utcDays = {
    {day("2024-03-09"), day("2024-03-10")},
    {day("2024-03-10"), day("2024-03-11")},
    {day("2024-03-11"), day("2024-03-12")},
  };
  const std::vector<int64_t> utcCounts = {1, 2, 1};
  for (auto [requestZone, facetZone] : {std::pair{"UTC", ""},
                                        std::pair{"America/Denver", ""},
                                        std::pair{"UTC", "America/Denver"},
                                        std::pair{"Asia/Kolkata", "+05:45"}}) {
    SCOPED_TRACE(std::string(requestZone) + " / " + facetZone);
    auto [bounds, counts] = facet("day_ld", requestZone, facetZone, api::CalendarGap_::Unit::DAY);
    EXPECT_EQ(utcDays, bounds);
    EXPECT_EQ(utcCounts, counts);
  }
  auto [weekBounds, weekCounts] =
      facet("day_ld", "America/Denver", "", api::CalendarGap_::Unit::WEEK);
  EXPECT_EQ((std::vector<std::pair<int64_t, int64_t>>{{day("2024-03-09"), day("2024-03-12")}}),
            weekBounds);
  EXPECT_EQ((std::vector<int64_t>{4}), weekCounts);

  // Contrast: the DATE field's UTC midnights are the previous Denver evening.
  auto [dateBounds, dateCounts] =
      facet("when_dt", "America/Denver", "", api::CalendarGap_::Unit::DAY);
  EXPECT_EQ((std::vector<int64_t>{2, 1, 0}), dateCounts);
}

TEST_F(LocalDateTest, sortsChronologically) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "a", "birthday_ld", "2020-03-15"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "b", "birthday_ld", "1995-01-01"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "c", "birthday_ld", "1969-06-01"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "d"), UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").timeZone("America/New_York").topDocs("q")
      .allQuery().fields({"id"}).limit(10);
  qb::sort(cur, "birthday_ld", qb::DESC);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  std::vector<std::string> ids;
  for (auto& doc : req->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id")));
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c", "d"}), ids);
}

// An explicit local_date field with a points index, and a local_date variant
// of a string field, behave like the template.
TEST_F(LocalDateTest, explicitFieldAndVariant) {
  CollectionHelper helper;
  SchemaBuilder b;
  auto& invoiced = b.field("invoiced");
  invoiced.type = FieldClass::LOCAL_DATE;
  invoiced.index = api::FieldDef::IndexMode::RANGE;
  auto& due = b.field("due");
  due.type = FieldClass::STRING;
  b.variant(due, "day").type = FieldClass::LOCAL_DATE;
  b.set(helper.collection());

  std::vector<Doc> docs = {
    flatdoc("id", "a", "invoiced", "2024-12-31", "due", "2025-01-31"),
    flatdoc("id", "b", "invoiced", "2024-12-31T18:00:00Z"),
    flatdoc("id", "c", "due", "2025-01-31T09:00"),
  };
  auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_EQ(Status::PARTIAL, result.status);
  ASSERT_EQ(2u, result.errors.size());

  EXPECT_EQ(1, count("Pacific/Auckland", [](OpCursor& c) { c.matchQuery("invoiced", "2024-12-31"); }));
  EXPECT_EQ(1, count("America/Los_Angeles", [](OpCursor& c) {
    c.exprQuery("due__day:[2025-01-01 TO 2025-01-31]");
  }));
}
