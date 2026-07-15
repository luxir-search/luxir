#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "solux/query/NumericRangeQuery.h"
#include "solux/query/QueryBuilder.h"
#include "solux/util/DateTime.h"

using namespace solux;
using namespace solux::test;

namespace {

TimeZone testZone(std::string_view name) {
  auto zone = resolveTimeZone(name);
  if (!zone) throw std::runtime_error(timeZoneResolutionError(name));
  return *zone;
}

int64_t utcMs(std::string_view text) {
  auto millis = parseDateToEpochMillis(text);
  if (!millis) throw std::runtime_error("invalid UTC test instant");
  return *millis;
}

DateRange zoneRange(std::string_view text, const TimeZone& zone, int64_t now = 0) {
  auto range = parseDateRange(text, now, zone);
  if (!range) throw std::runtime_error("invalid zoned test date: " + std::string(text));
  return *range;
}

void expectZoneRange(std::string_view text, const TimeZone& zone,
                     std::string_view lo, std::string_view hi, int64_t now = 0) {
  DateRange range = zoneRange(text, zone, now);
  EXPECT_EQ(utcMs(lo), range.lo) << text;
  EXPECT_EQ(utcMs(hi), range.hiExclusive) << text;
}

} // namespace

class DateFieldTest : public SoluxTest {
};

// ---- parse: strict_date_optional_time || epoch_millis (Solr/OpenSearch) ----

TEST_F(DateFieldTest, parseAnchors) {
  // Epoch and millisecond resolution.
  EXPECT_EQ(0, parseDateToEpochMillis("1970-01-01T00:00:00Z"));
  EXPECT_EQ(1, parseDateToEpochMillis("1970-01-01T00:00:00.001Z"));
  EXPECT_EQ(-1000, parseDateToEpochMillis("1969-12-31T23:59:59Z"));
  // Well-known anchor: 2000-01-01T00:00:00Z == 946684800000 ms.
  EXPECT_EQ(946684800000LL, parseDateToEpochMillis("2000-01-01T00:00:00Z"));
}

TEST_F(DateFieldTest, parseOptionalTimeAndForms) {
  auto z = parseDateToEpochMillis("2000-01-01T00:00:00Z");
  ASSERT_TRUE(z.has_value());
  // Date-only and truncated-time forms all anchor to UTC midnight / minute.
  EXPECT_EQ(z, parseDateToEpochMillis("2000-01-01"));        // strict_date_optional_time: time optional
  EXPECT_EQ(z, parseDateToEpochMillis("2000-01-01T00:00"));  // seconds optional
  EXPECT_EQ(z, parseDateToEpochMillis("2000-01-01T00:00:00"));  // zone optional (defaults UTC)
  EXPECT_EQ(*z + 86400000LL, parseDateToEpochMillis("2000-01-02"));  // one day later
  EXPECT_EQ(*z + 3600000LL, parseDateToEpochMillis("2000-01-01T01:00:00Z"));
}

TEST_F(DateFieldTest, parseTimezoneOffsets) {
  auto z = *parseDateToEpochMillis("2000-01-01T12:00:00Z");
  // A +01:00 local time is one hour earlier in UTC; -05:00 is five hours later.
  EXPECT_EQ(z - 3600000LL, parseDateToEpochMillis("2000-01-01T12:00:00+01:00"));
  EXPECT_EQ(z + 5 * 3600000LL, parseDateToEpochMillis("2000-01-01T12:00:00-05:00"));
  EXPECT_EQ(z - 3600000LL, parseDateToEpochMillis("2000-01-01T12:00:00+0100"));  // no colon
}

TEST_F(DateFieldTest, parseFractionTruncates) {
  auto z = *parseDateToEpochMillis("2000-01-01T00:00:00Z");
  EXPECT_EQ(z + 123, parseDateToEpochMillis("2000-01-01T00:00:00.123Z"));
  EXPECT_EQ(z + 500, parseDateToEpochMillis("2000-01-01T00:00:00.5Z"));
  // Sub-millisecond digits are truncated, not rounded.
  EXPECT_EQ(z + 123, parseDateToEpochMillis("2000-01-01T00:00:00.123999Z"));
}

TEST_F(DateFieldTest, parseEpochMillisInteger) {
  // OpenSearch epoch_millis: a bare (optionally signed) integer string.
  EXPECT_EQ(946684800000LL, parseDateToEpochMillis("946684800000"));
  EXPECT_EQ(-1000, parseDateToEpochMillis("-1000"));
  EXPECT_EQ(0, parseDateToEpochMillis("0"));
}

TEST_F(DateFieldTest, parseSolrDateMath) {
  int64_t now = *parseDateToEpochMillis("2024-06-25T10:30:45.123Z");
  EXPECT_EQ(now, parseDateToEpochMillis("NOW", now));
  EXPECT_EQ(*parseDateToEpochMillis("2024-08-22T00:00:00Z"),
            parseDateToEpochMillis("NOW+2MONTHS-3DAYS/DAY", now));
  EXPECT_EQ(*parseDateToEpochMillis("1972-11-23T00:00:00Z"),
            parseDateToEpochMillis(
                "1972-05-20T17:33:18.772Z+6MONTHS+3DAYS/DAY", now));

  // Calendar addition clamps rather than rolling into the following month.
  EXPECT_EQ(*parseDateToEpochMillis("2024-02-29T12:00:00Z"),
            parseDateToEpochMillis("2024-01-31T12:00:00Z+1MONTH", now));
  EXPECT_EQ(*parseDateToEpochMillis("2025-02-28T12:00:00Z"),
            parseDateToEpochMillis("2024-02-29T12:00:00Z+1YEAR", now));
  EXPECT_EQ(now + datetime_detail::kMsPerDay + 60001,
            parseDateToEpochMillis("NOW+1DATE+1MINUTE+1MILLI", now));
  // The longest valid anchor wins, including its numeric zone offset.
  EXPECT_EQ(*parseDateToEpochMillis("2024-06-25T09:30:00Z"),
            parseDateToEpochMillis("2024-06-25T10:30:00+02:00+1HOUR", now));
}

TEST_F(DateFieldTest, parseOpenSearchDateMath) {
  int64_t now = *parseDateToEpochMillis("2024-06-25T10:30:45.123Z");
  EXPECT_EQ(*parseDateToEpochMillis("2024-07-25T10:30:45.123Z"),
            parseDateToEpochMillis("now+1M", now));
  EXPECT_EQ(now + 60000, parseDateToEpochMillis("now+1m", now));
  EXPECT_EQ(now + 3601000, parseDateToEpochMillis("now+1H+1s", now));
  EXPECT_EQ(*parseDateToEpochMillis("2022-07-17T00:00:00Z"),
            parseDateToEpochMillis("2022-05-18T15:23:17.789||+2M-1d/d", now));

  auto day = parseDateRange("2022-05-18T15:23||/d", now, TimeZone::utc());
  ASSERT_TRUE(day.has_value());
  EXPECT_EQ(*parseDateToEpochMillis("2022-05-18T00:00:00Z"), day->lo);
  EXPECT_EQ(*parseDateToEpochMillis("2022-05-19T00:00:00Z"), day->hiExclusive);

  auto week = parseDateRange("2024-06-26T12:00:00Z||/w", now, TimeZone::utc());  // Wednesday
  ASSERT_TRUE(week.has_value());
  EXPECT_EQ(*parseDateToEpochMillis("2024-06-24T00:00:00Z"), week->lo);  // Monday
  EXPECT_EQ(*parseDateToEpochMillis("2024-07-01T00:00:00Z"), week->hiExclusive);
}

TEST_F(DateFieldTest, dateMathRoundingWindowAndAnchorCollision) {
  int64_t now = *parseDateToEpochMillis("2024-06-25T10:30:45.123Z");
  auto shiftedDay = parseDateRange("NOW/DAY+1HOUR", now, TimeZone::utc());
  ASSERT_TRUE(shiftedDay.has_value());
  EXPECT_EQ(*parseDateToEpochMillis("2024-06-25T01:00:00Z"), shiftedDay->lo);
  EXPECT_EQ(*parseDateToEpochMillis("2024-06-26T01:00:00Z"), shiftedDay->hiExclusive);

  // No math preserves the existing partial-literal window. Once math starts,
  // the partial anchor is its start instant; only slash creates a new window.
  auto june = parseDateRange("2024-06", now, TimeZone::utc());
  auto july = parseDateRange("2024-06||+1M", now, TimeZone::utc());
  ASSERT_TRUE(june.has_value());
  ASSERT_TRUE(july.has_value());
  EXPECT_EQ(30 * datetime_detail::kMsPerDay, june->hiExclusive - june->lo);
  EXPECT_EQ(1, july->hiExclusive - july->lo);
  EXPECT_EQ(*parseDateToEpochMillis("2024-07-01T00:00:00Z"), july->lo);

  // Solr direct suffixes remain available on partial anchors too.
  EXPECT_EQ(*parseDateToEpochMillis("2024-05-31T00:00:00Z"),
            parseDateToEpochMillis("2024-06-1DAY", now));
}

TEST_F(DateFieldTest, dateMathRejectsMalformedAndOverflow) {
  int64_t now = *parseDateToEpochMillis("2024-06-25T10:30:45.123Z");
  for (std::string_view bad : {
         "NOW+DAY", "NOW+1", "NOW/", "NOW+1FORTNIGHT", "NOW +1DAY",
         "NOW||", "2024-01-01||", "2024-01-01||||+1d",
         "now+1D", "now+9223372036854775807y",
         "32767-12-31T23:59:59.999Z+1MILLI"
       }) {
    EXPECT_FALSE(parseDateRange(bad, now, TimeZone::utc()).has_value()) << bad;
  }
}

TEST(DateMathQueryBuilder, usesOneExplicitNowAndRoundingWindow) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  int64_t now = *parseDateToEpochMillis("2024-06-25T10:30:45.123Z");
  QueryBuilder builder(pool, *schema, CoerceContext{now, TimeZone::utc()});
  api::Val val;
  val.kind = std::string_view("NOW/DAY");
  auto* range = dynamic_cast<NumericRangeQuery*>(
      builder.createMatchQuery("when_dt", val));
  ASSERT_NE(nullptr, range);
  EXPECT_EQ(*parseDateToEpochMillis("2024-06-25T00:00:00Z"), range->getLo());
  EXPECT_EQ(*parseDateToEpochMillis("2024-06-25T23:59:59.999Z"), range->getHi());
}

TEST_F(DateFieldTest, parseRejectsInvalid) {
  EXPECT_FALSE(parseDateToEpochMillis("").has_value());
  EXPECT_FALSE(parseDateToEpochMillis("not a date").has_value());
  EXPECT_FALSE(parseDateToEpochMillis("2000-13-01").has_value());      // month 13
  EXPECT_FALSE(parseDateToEpochMillis("2001-02-29").has_value());      // not a leap year
  EXPECT_FALSE(parseDateToEpochMillis("2000-01-01T25:00:00Z").has_value());  // hour 25
  EXPECT_FALSE(parseDateToEpochMillis("2000-01-01 extra").has_value());      // trailing junk
  EXPECT_FALSE(parseDateToEpochMillis("2000/01/01").has_value());      // wrong separators
  // 2000 IS a leap year (divisible by 400), so Feb 29 is valid.
  EXPECT_TRUE(parseDateToEpochMillis("2000-02-29").has_value());
}

// Guard against silent wraparound: int64 overflow on the bare-int path and
// years past std::chrono::year's 16-bit range must be rejected, not wrapped
// into a plausible-but-wrong instant.
TEST_F(DateFieldTest, parseRejectsOutOfRange) {
  // bare integer beyond int64 (would be signed-overflow UB if hand-accumulated)
  EXPECT_FALSE(parseDateToEpochMillis("99999999999999999999").has_value());   // 20 digits
  EXPECT_FALSE(parseDateToEpochMillis("-99999999999999999999").has_value());
  // bare integer within int64 but past the supported instant range
  EXPECT_FALSE(parseDateToEpochMillis("9000000000000000000").has_value());    // ~9e18 ms
  // ISO year past chrono::year's range (would narrow silently and pass ok())
  EXPECT_FALSE(parseDateToEpochMillis("100000-01-01").has_value());
  // boundary still parses (year 9999 ~ 2.5e14 ms, well inside the range)
  EXPECT_TRUE(parseDateToEpochMillis("9999-12-31T23:59:59Z").has_value());
  auto maxMonth = parseDateRange("32767-12", currentEpochMillis(), TimeZone::utc());
  ASSERT_TRUE(maxMonth.has_value());
  EXPECT_EQ(datetime_detail::kMaxEpochMs + 1, maxMonth->hiExclusive);
  EXPECT_FALSE(parseDateToEpochMillis("32767-12-31T23:59:59-01:00").has_value());
}

// Date strings are untrusted user input, so the parser is an attack surface.
// Throw random (including malformed, high-byte, embedded-NUL, max-length)
// strings at it: it must never read out of bounds (ASan/UBSan catches that),
// and any value it DOES accept must lie in the supported range and round-trip
// through the formatter.  Biased toward date-shaped bytes so the structured
// paths are exercised, not just early rejects.
TEST_F(DateFieldTest, parseFuzzSafetyAndInvariants) {
  static const char alphabet[] = "0123456789-:.TtZz+ ,/|NOWymwdhHsMILLISECONDS";
  std::string buf;
  for (int iter = 0; iter < 100000; iter++) {
    size_t len = iter % 100 == 0 ? 257 : rng() % 72;  // regularly cross the 256-byte cap
    buf.resize(len);
    for (size_t i = 0; i < len; i++) {
      buf[i] = (rng() & 3) ? alphabet[rng() % (sizeof(alphabet) - 1)] : (char)(rng() & 0xff);
    }
    auto r = parseDateToEpochMillis(buf);  // must not crash on any input
    if (r) {
      ASSERT_GE(*r, datetime_detail::kMinEpochMs) << "input='" << buf << "'";
      ASSERT_LE(*r, datetime_detail::kMaxEpochMs) << "input='" << buf << "'";
      ASSERT_EQ(*r, parseDateToEpochMillis(formatEpochMillisIso8601(*r))) << "input='" << buf << "'";
    }
  }
}

// DATE columns merge through the int-column path (mergeIntCol2); values and
// chronological sort order must survive a multi-segment force-merge.
TEST_F(DateFieldTest, mergeAcrossSegments) {
  CollectionHelper helper;

  int64_t a = *parseDateToEpochMillis("1980-05-05T00:00:00Z");
  int64_t b = *parseDateToEpochMillis("2010-10-10T00:00:00Z");
  int64_t c = *parseDateToEpochMillis("1969-01-01T00:00:00Z");  // pre-epoch

  helper.index(flatdoc("id_s", "a", "when_dt", a), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "b", "when_dt", "2010-10-10T00:00:00Z"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "when_dt", c), UpdateMessage::COMMIT);
  helper.getIndexWriter()->mergeSegments();
  helper.commit();

  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("main").topDocs("q").allQuery().fields({"id_s", "when_dt"}).limit(10);
  lreq->execute();
  auto docs = lreq->getDocs();
  ASSERT_EQ(3u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "when_dt", a)));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id_s", "b", "when_dt", b)));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id_s", "c", "when_dt", c)));

  auto s = localReq(soluxNode->getSearchEngine());
  auto& scur = s->collection("main").topDocs("q").allQuery().fields({"id_s"}).limit(10);
  qb::sort(scur, "when_dt", qb::ASC);
  s->execute();
  std::vector<std::string> ids;
  for (auto& d : s->getDocs()) ids.push_back(std::get<std::string>(*find(d, "id_s")));
  EXPECT_EQ((std::vector<std::string>{"c", "a", "b"}), ids);
}

// ---- format: canonical UTC ISO-8601, '.fff' only when non-zero ----

TEST_F(DateFieldTest, formatCanonical) {
  EXPECT_EQ("1970-01-01T00:00:00Z", formatEpochMillisIso8601(0));
  EXPECT_EQ("1970-01-01T00:00:00.001Z", formatEpochMillisIso8601(1));
  EXPECT_EQ("2000-01-01T00:00:00Z", formatEpochMillisIso8601(946684800000LL));
  EXPECT_EQ("1969-12-31T23:59:59Z", formatEpochMillisIso8601(-1000));  // pre-epoch (floor)
  EXPECT_EQ("1969-12-31T23:59:59.500Z", formatEpochMillisIso8601(-500));
}

TEST_F(DateFieldTest, parseFormatRoundTrip) {
  for (int64_t ms : {0LL, 1LL, -1LL, 1000LL, -1000LL, 946684800000LL,
                     1750000000000LL, -62135596800000LL, 253402300799000LL}) {
    EXPECT_EQ(ms, parseDateToEpochMillis(formatEpochMillisIso8601(ms))) << "ms=" << ms;
  }
}

// ---- end to end: index (string + epoch int), retrieve (epoch millis col) ----

TEST_F(DateFieldTest, roundTrip) {
  CollectionHelper helper;

  int64_t ms1 = *parseDateToEpochMillis("2026-06-24T12:00:00Z");
  int64_t ms2 = *parseDateToEpochMillis("1999-12-31T23:59:59.250Z");
  int64_t a = *parseDateToEpochMillis("2001-01-01");
  int64_t b = *parseDateToEpochMillis("2002-02-02");

  // d1: single date as an ISO string; multi dates as ISO strings.
  helper.index(flatdoc("id_s", "d1", "when_dt", "2026-06-24T12:00:00Z",
                       "stamps_dts", std::vector<std::string>{"2001-01-01", "2002-02-02"}),
               UpdateMessage::NO_COMMIT);
  // d2: single date as raw epoch millis (int passthrough); multi as epoch ints.
  helper.index(flatdoc("id_s", "d2", "when_dt", ms2,
                       "stamps_dts", vec_i(a, b)),
               UpdateMessage::COMMIT);

  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("main").topDocs("q")
      .allQuery()
      .fields({"id_s", "when_dt", "stamps_dts"})
      .limit(10);
  lreq->execute();

  auto docs = lreq->getDocs();
  ASSERT_EQ(2u, docs.size());
  // DATE columns come back as raw epoch millis (int64), regardless of input form.
  EXPECT_TRUE(containsDoc(docs, flatdoc("id_s", "d1", "when_dt", ms1,
                                        "stamps_dts", vec_i(a, b))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id_s", "d2", "when_dt", ms2,
                                        "stamps_dts", vec_i(a, b))));
}

TEST_F(DateFieldTest, indexDateMathAndStableNowPerUpdate) {
  CollectionHelper helper;
  std::vector<Doc> docs = {
    flatdoc("id", "solr", "when_dt", "2024-06-24T12:00:00Z+1DAY"),
    flatdoc("id", "opensearch", "when_dt", "2024-06-24T12:00:00Z||+1d"),
    flatdoc("id", "now1", "when_dt", "NOW"),
    flatdoc("id", "now2", "when_dt", "now"),
  };
  auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_EQ(solux::api::UpdateResponse_::Status::OK, result.status);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "when_dt"}).limit(10);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();

  int64_t expected = *parseDateToEpochMillis("2024-06-25T12:00:00Z");
  std::optional<int64_t> now1, now2;
  for (auto& doc : req->getDocs()) {
    auto* id = find(doc, "id");
    auto* when = find(doc, "when_dt");
    ASSERT_NE(nullptr, id);
    ASSERT_NE(nullptr, when);
    std::string value = std::get<std::string>(*id);
    int64_t millis = std::get<int64_t>(*when);
    if (value == "solr" || value == "opensearch") {
      EXPECT_EQ(expected, millis);
    }
    if (value == "now1") now1 = millis;
    if (value == "now2") now2 = millis;
  }
  ASSERT_TRUE(now1.has_value());
  ASSERT_TRUE(now2.has_value());
  EXPECT_EQ(*now1, *now2);
}

// Sorting runs on the raw millis column (chronological order, no decode).
TEST_F(DateFieldTest, sort) {
  CollectionHelper helper;

  helper.index(flatdoc("id_s", "a", "when_dt", "2020-03-15T00:00:00Z"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "when_dt", "1995-01-01T00:00:00Z"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "when_dt", "1969-06-01T00:00:00Z"), UpdateMessage::NO_COMMIT);  // pre-epoch
  helper.index(flatdoc("id_s", "d"), UpdateMessage::COMMIT);  // missing, sorts last asc

  auto lreq = localReq(soluxNode->getSearchEngine());
  auto& cur = lreq->collection("main").topDocs("q").allQuery().fields({"id_s"}).limit(10);
  qb::sort(cur, "when_dt", qb::ASC);
  lreq->execute();

  std::vector<std::string> ids;
  for (auto& doc : lreq->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id_s")));
  EXPECT_EQ((std::vector<std::string>{"c", "b", "a", "d"}), ids);
}

// A multi-valued cell whose Nth string fails to parse must fail the whole
// doc WITHOUT leaving the already-parsed earlier values orphaned in the
// column's value stream - columns reconstruct positionally, so an orphan
// would shift every later doc's values.  The failed doc sits in the middle
// with a valid leading value, and we assert the trailing good doc reads
// back exactly its own values.
TEST_F(DateFieldTest, multiStringPartialFailureNoCorruption) {
  CollectionHelper helper;

  int64_t y2001 = *parseDateToEpochMillis("2001-01-01");
  int64_t y2003 = *parseDateToEpochMillis("2003-03-03");
  int64_t y2004 = *parseDateToEpochMillis("2004-04-04");

  std::vector<Doc> docs = {
    flatdoc("id", "g1", "stamps_dts", std::vector<std::string>{"2001-01-01"}),
    // first element valid (would orphan one value), second element unparseable
    flatdoc("id", "b1", "stamps_dts", std::vector<std::string>{"2002-02-02", "not-a-date"}),
    flatdoc("id", "g2", "stamps_dts", std::vector<std::string>{"2003-03-03", "2004-04-04"}),
  };
  auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_EQ(solux::api::UpdateResponse_::Status::PARTIAL, result.status);
  ASSERT_EQ(1, result.errors.size());
  EXPECT_EQ("b1", result.errors[0].id);

  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("main").topDocs("q").allQuery().fields({"id", "stamps_dts"}).limit(10);
  lreq->execute();
  auto retrieved = lreq->getDocs();
  // The good docs must read back exactly their own values (no shift from b1).
  EXPECT_TRUE(containsDoc(retrieved, flatdoc("id", "g1", "stamps_dts", vec_i(y2001))));
  EXPECT_TRUE(containsDoc(retrieved, flatdoc("id", "g2", "stamps_dts", vec_i(y2003, y2004))));
}

// Query-side granularity: a date literal denotes the window it names.
TEST_F(DateFieldTest, parseGranularityWindows) {
  auto day = parseDateRange("2024-06-25", currentEpochMillis(), TimeZone::utc());
  ASSERT_TRUE(day.has_value());
  EXPECT_EQ(day->lo, *parseDateToEpochMillis("2024-06-25T00:00:00Z"));
  EXPECT_EQ(day->hiExclusive, *parseDateToEpochMillis("2024-06-26T00:00:00Z"));

  auto month = parseDateRange("2024-02", currentEpochMillis(), TimeZone::utc());  // leap February
  ASSERT_TRUE(month.has_value());
  EXPECT_EQ(29 * 86400000LL, month->hiExclusive - month->lo);
  auto dec = parseDateRange("2024-12", currentEpochMillis(), TimeZone::utc());    // year rollover
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->hiExclusive, *parseDateToEpochMillis("2025-01-01"));

  auto minute = parseDateRange("2024-06-25T10:30", currentEpochMillis(), TimeZone::utc());
  ASSERT_TRUE(minute.has_value());
  EXPECT_EQ(60000, minute->hiExclusive - minute->lo);
  auto instant = parseDateRange("2024-06-25T10:30:00.123Z", currentEpochMillis(), TimeZone::utc());
  ASSERT_TRUE(instant.has_value());
  EXPECT_EQ(1, instant->hiExclusive - instant->lo);
  auto epoch = parseDateRange("1700000000000", currentEpochMillis(), TimeZone::utc());
  ASSERT_TRUE(epoch.has_value());
  EXPECT_EQ(1, epoch->hiExclusive - epoch->lo);

  // a zone offset shifts the whole window
  auto offs = parseDateRange("2024-06-25T10-07:00", currentEpochMillis(), TimeZone::utc());
  ASSERT_TRUE(offs.has_value());
  EXPECT_EQ(3600000, offs->hiExclusive - offs->lo);
  EXPECT_EQ(offs->lo, *parseDateToEpochMillis("2024-06-25T17:00:00Z"));

  // a time needs a day: month + time stays invalid
  EXPECT_FALSE(parseDateRange("2024-06T10", currentEpochMillis(), TimeZone::utc()).has_value());
}

TEST_F(DateFieldTest, timeZoneGrammarAndTzdbResolution) {
  for (std::string_view spec : {"", "Z", "UTC", "+00", "-00:00"}) {
    auto zone = resolveTimeZone(spec);
    ASSERT_TRUE(zone.has_value()) << spec;
    EXPECT_TRUE(zone->isUtc()) << spec;
  }
  for (std::string_view spec : {"+05:30", "+0530", "+05", "+18:00"}) {
    auto zone = resolveTimeZone(spec);
    ASSERT_TRUE(zone.has_value()) << spec;
    EXPECT_TRUE(zone->isFixed()) << spec;
  }
  EXPECT_EQ(330, resolveTimeZone("+05:30")->offsetMinutes());
  EXPECT_EQ(1080, resolveTimeZone("+18:00")->offsetMinutes());

  std::string embeddedNul("America/Denver\0junk", 19);
  for (std::string_view spec : {"utc", "z", "+18:01", "+24:00", "+5:30",
                                " UTC", "UTC ", "No/Such_Zone"}) {
    EXPECT_FALSE(resolveTimeZone(spec).has_value()) << spec;
  }
  EXPECT_FALSE(resolveTimeZone(embeddedNul).has_value());

  auto alias = resolveTimeZone("EST5EDT");
  ASSERT_TRUE(alias.has_value());
  EXPECT_TRUE(alias->isIana());
  EXPECT_EQ("America/New_York", alias->name());  // chrono returns the canonical target
  EXPECT_TRUE(timeZoneDatabaseAvailable());
  EXPECT_FALSE(timeZoneDatabaseVersion().empty());

  std::atomic<bool> concurrentOk = true;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < 100; ++j) {
        auto zone = resolveTimeZone("America/Denver");
        if (!zone || !zone->isIana()) concurrentOk = false;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_TRUE(concurrentOk);
}

TEST_F(DateFieldTest, literalOffsetPresenceCapsAndFrameRebase) {
  TimeZone denver = testZone("America/Denver");
  auto absent = parseDateLiteralRange("2024-01-15T00:00:00", denver);
  auto zulu = parseDateLiteralRange("2024-01-15T00:00:00Z", denver);
  auto plusZero = parseDateLiteralRange("2024-01-15T00:00:00+00:00", denver);
  ASSERT_TRUE(absent && zulu && plusZero);
  EXPECT_FALSE(absent->hasExplicitOffset);
  EXPECT_TRUE(zulu->hasExplicitOffset);
  EXPECT_TRUE(plusZero->hasExplicitOffset);
  EXPECT_EQ(utcMs("2024-01-15T07:00:00Z"), absent->range.lo);
  EXPECT_EQ(utcMs("2024-01-15T00:00:00Z"), zulu->range.lo);
  EXPECT_EQ(zulu->range.lo, plusZero->range.lo);

  for (std::string_view offset : {"+18:01", "+23:59", "-23:59"}) {
    EXPECT_TRUE(parseDateLiteralRange(
        std::string("2024-01-15T00:00:00") + std::string(offset), denver).has_value())
        << offset;
  }
  EXPECT_FALSE(parseDateLiteralRange("2024-01-15T00:00:00+24:00", denver).has_value());

  // The +20:00 token locates the anchor only. /DAY first rebases that instant
  // into Denver (2024-03-09T02:30), so the result starts at Denver midnight.
  expectZoneRange("2024-03-10T05:30:00+20:00/DAY", denver,
                  "2024-03-09T07:00:00Z", "2024-03-10T07:00:00Z");
}

// Historical IANA assertions below depend on tzdb data in principle. They pin
// politically stable, already-observed transitions (tzdb 2026a on this box).
TEST_F(DateFieldTest, denverSpringForwardOfLocalAndDateMath) {
  TimeZone denver = testZone("America/Denver");
  expectZoneRange("2024-03-10T02", denver,
                  "2024-03-10T09:00:00Z", "2024-03-10T10:00:00Z");
  expectZoneRange("2024-03-10T02:30", denver,
                  "2024-03-10T09:30:00Z", "2024-03-10T09:31:00Z");
  expectZoneRange("2024-03-10T02:30:30", denver,
                  "2024-03-10T09:30:30Z", "2024-03-10T09:30:31Z");

  expectZoneRange("2024-03-10T12:00:00/DAY", denver,
                  "2024-03-10T07:00:00Z", "2024-03-11T06:00:00Z");
  expectZoneRange("2024-03-10T12:00:00/WEEK", denver,
                  "2024-03-04T07:00:00Z", "2024-03-11T06:00:00Z");
  expectZoneRange("2024-03-10T12:00:00/MONTH", denver,
                  "2024-03-01T07:00:00Z", "2024-04-01T06:00:00Z");

  int64_t noonBefore = utcMs("2024-03-09T19:00:00Z");  // Denver 12:00 MST
  EXPECT_EQ(utcMs("2024-03-10T18:00:00Z"),
            zoneRange("NOW+1DAY", denver, noonBefore).lo);
  EXPECT_EQ(utcMs("2024-03-10T19:00:00Z"),
            zoneRange("NOW+24HOURS", denver, noonBefore).lo);
  EXPECT_EQ(utcMs("2024-03-09T18:00:00Z"),
            zoneRange("NOW+1DAY-24HOURS", denver, noonBefore).lo);
  EXPECT_EQ(noonBefore + 3600000,
            zoneRange("NOW+1HOUR", denver, noonBefore).lo);
  EXPECT_EQ(noonBefore + 3600000,
            zoneRange("NOW+60MINUTES", denver, noonBefore).lo);

  EXPECT_EQ(utcMs("2024-03-10T09:30:00Z"),
            zoneRange("2024-03-09T02:30+1DAY", denver).lo);
  EXPECT_EQ(utcMs("2024-03-09T10:30:00Z"),
            zoneRange("2024-03-09T02:30+1DAY-1DAY", denver).lo);
  EXPECT_EQ(utcMs("2024-03-11T09:30:00Z"),
            zoneRange("2024-03-09T02:30+1DAY+1DAY", denver).lo);
}

TEST_F(DateFieldTest, denverSpringGapGranuleWarnings) {
  TimeZone denver = testZone("America/Denver");
  EXPECT_TRUE(zoneRange("2024-03-10T02:30", denver).granuleSkipped);
  EXPECT_TRUE(zoneRange("2024-03-10T02", denver).granuleSkipped);
  EXPECT_FALSE(zoneRange("2024-03-10", denver).granuleSkipped);
  EXPECT_FALSE(zoneRange("2024-03-09T02:30", denver).granuleSkipped);

  CollectionHelper helper;
  helper.index(flatdoc("id", "gap", "when_dt", "2024-03-10T09:30:00Z"),
               UpdateMessage::COMMIT);
  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main").timeZone("America/Denver").topDocs("q")
      .matchQuery("when_dt", "2024-03-10T02:30").withStats();
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(1, req->getMatchCount());
  EXPECT_TRUE(req->hasWarning("date_granule_skipped"));
}

TEST_F(DateFieldTest, denverFallBackFoldRetentionAndLastMillisecond) {
  TimeZone denver = testZone("America/Denver");
  int64_t firstFold = utcMs("2024-11-03T07:30:00Z");
  int64_t secondFold = utcMs("2024-11-03T08:30:00Z");
  expectZoneRange("NOW/HOUR", denver,
                  "2024-11-03T07:00:00Z", "2024-11-03T08:00:00Z", firstFold);
  expectZoneRange("NOW/HOUR", denver,
                  "2024-11-03T08:00:00Z", "2024-11-03T09:00:00Z", secondFold);
  EXPECT_EQ(firstFold, zoneRange("NOW+0DAY", denver, firstFold).lo);
  EXPECT_EQ(secondFold, zoneRange("NOW+0DAY", denver, secondFold).lo);

  expectZoneRange("2024-11-03T01", denver,
                  "2024-11-03T07:00:00Z", "2024-11-03T08:00:00Z");
  expectZoneRange("2024-11-03", denver,
                  "2024-11-03T06:00:00Z", "2024-11-04T07:00:00Z");
  EXPECT_EQ(secondFold,
            zoneRange("2024-11-03T01:30:00-07:00", denver).lo);
  expectZoneRange("2024-11-03T01:30:00-07:00/DAY", denver,
                  "2024-11-03T06:00:00Z", "2024-11-04T07:00:00Z");
}

TEST_F(DateFieldTest, politicalBoundaryRounding) {
  TimeZone saoPaulo = testZone("America/Sao_Paulo");
  expectZoneRange("2018-11-04T00:30", saoPaulo,
                  "2018-11-04T03:30:00Z", "2018-11-04T03:31:00Z");
  expectZoneRange("2018-11-04T00:30/DAY", saoPaulo,
                  "2018-11-04T03:00:00Z", "2018-11-05T02:00:00Z");

  TimeZone havana = testZone("America/Havana");
  expectZoneRange("2015-11-01T00:30/MONTH", havana,
                  "2015-11-01T04:00:00Z", "2015-12-01T05:00:00Z");
  expectZoneRange("2015-10-15T12:00/MONTH", havana,
                  "2015-10-01T04:00:00Z", "2015-11-01T04:00:00Z");

  TimeZone kathmandu = testZone("Asia/Kathmandu");
  expectZoneRange("1986-07-01T12:00/YEAR", kathmandu,
                  "1985-12-31T18:30:00Z", "1986-12-31T18:15:00Z");
}

TEST_F(DateFieldTest, exoticOffsetsAndSubMinuteHistory) {
  TimeZone lordHowe = testZone("Australia/Lord_Howe");
  expectZoneRange("2024-10-06T02:15", lordHowe,
                  "2024-10-05T15:45:00Z", "2024-10-05T15:46:00Z");
  EXPECT_EQ(utcMs("2024-04-06T14:45:00Z"),
            zoneRange("2024-04-07T01:45", lordHowe).lo);
  EXPECT_EQ(utcMs("2024-04-06T15:15:00Z"),
            zoneRange("2024-04-07T01:45+10:30", lordHowe).lo);

  expectZoneRange("2024-06-01T00:00", testZone("Pacific/Chatham"),
                  "2024-05-31T11:15:00Z", "2024-05-31T11:16:00Z");
  expectZoneRange("2024-01-01T00:00", testZone("America/St_Johns"),
                  "2024-01-01T03:30:00Z", "2024-01-01T03:31:00Z");

  // Liberia used -00:44:30 through 1971. The IANA path must retain seconds;
  // fixed request offsets intentionally remain minute-granular.
  expectZoneRange("1970-01-01T00:00", testZone("Africa/Monrovia"),
                  "1970-01-01T00:44:30Z", "1970-01-01T00:45:30Z");
}

TEST_F(DateFieldTest, fixedOffsetAndIanaEquivalence) {
  TimeZone fixed = testZone("+05:00");
  TimeZone iana = testZone("Etc/GMT-5");
  for (std::string_view expression : {
         "2024-03-10", "2024-03-10T02:30", "2024-03-10T02:30/DAY",
         "2024-01-31T12:30+1MONTH", "2024-02-29T12:30+1YEAR",
         "2024-03-10T02:30+1DAY+3HOURS", "2024-03-10T02:30/WEEK"}) {
    DateRange a = zoneRange(expression, fixed);
    DateRange b = zoneRange(expression, iana);
    EXPECT_EQ(a.lo, b.lo) << expression;
    EXPECT_EQ(a.hiExclusive, b.hiExclusive) << expression;
  }
}

TEST_F(DateFieldTest, civilYearGuardAtInstantCaps) {
  using namespace datetime_detail;
  TimeZone plus14 = testZone("+14:00");
  TimeZone minus12 = testZone("-12:00");
  EXPECT_TRUE(civilFromInstant(kMinEpochMs, TimeZone::utc()).has_value());
  EXPECT_TRUE(civilFromInstant(kMaxEpochMs, TimeZone::utc()).has_value());
  EXPECT_TRUE(civilFromInstant(kMinEpochMs, plus14).has_value());
  EXPECT_FALSE(civilFromInstant(kMaxEpochMs, plus14).has_value());
  EXPECT_FALSE(civilFromInstant(kMinEpochMs, minus12).has_value());
  EXPECT_TRUE(civilFromInstant(kMaxEpochMs, minus12).has_value());

  std::string min = std::to_string(kMinEpochMs);
  std::string max = std::to_string(kMaxEpochMs);
  EXPECT_TRUE(parseDateRange(min, 0, plus14).has_value());  // epoch is zone-free
  EXPECT_TRUE(parseDateRange(max, 0, plus14).has_value());
  EXPECT_TRUE(parseDateLiteralRange("32767-12-31T00:00", plus14).has_value());
  EXPECT_TRUE(parseDateLiteralRange("-32767-01-01T00:00", minus12).has_value());
  EXPECT_FALSE(parseDateRange(max + "||/DAY", 0, plus14).has_value());
  EXPECT_FALSE(parseDateRange(min + "||/YEAR", 0, minus12).has_value());
  EXPECT_FALSE(parseDateRange(max + "||-1YEAR", 0, plus14).has_value());
  EXPECT_FALSE(addUnit(kMaxEpochMs - 14 * 3600000LL, 1,
                       DateMathUnit::YEAR, plus14).has_value());
  EXPECT_FALSE(addUnit(kMinEpochMs + 12 * 3600000LL, -1,
                       DateMathUnit::YEAR, minus12).has_value());

  TimeZone namedPlus14 = testZone("Etc/GMT-14");
  TimeZone namedMinus12 = testZone("Etc/GMT+12");
  EXPECT_FALSE(civilFromInstant(kMaxEpochMs, namedPlus14).has_value());
  EXPECT_FALSE(civilFromInstant(kMinEpochMs, namedMinus12).has_value());
}

TEST_F(DateFieldTest, requestZoneThreadsAllQueryDialectsAndBounds) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "a", "when_dt", "2024-03-10T09:30:00Z"),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "b", "when_dt", "2024-03-10T09:30:59.999Z"),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "c", "when_dt", "2024-03-10T09:31:00Z"),
               UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main").timeZone("America/Denver");
  auto& structured = req->topDocs("structured");
  structured.rawQuery() = qb::range(
      structured.mr(), "when_dt", qb::valStr(structured.mr(), "2024-03-10T02:30"),
      nullptr, qb::valStr(structured.mr(), "2024-03-10T02:30"), nullptr);
  structured.withStats();
  req->topDocs("expr").exprQuery("when_dt:[2024-03-10T02:30 TO 2024-03-10T02:30]")
      .withStats();
  req->topDocs("simple").simpleQuery("when_dt:2024-03-10T02:30", {"id"})
      .withStats();
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(2, req->getMatchCount("structured"));
  EXPECT_EQ(2, req->getMatchCount("expr"));
  EXPECT_EQ(2, req->getMatchCount("simple"));
}

TEST_F(DateFieldTest, ingestRemainsUtcUnderZonedQueries) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "utc", "when_dt", "2024-01-15"), UpdateMessage::COMMIT);

  auto utc = localReq(soluxNode->getSearchEngine());
  utc->collection("main").topDocs("q").matchQuery("when_dt", "2024-01-15").withStats();
  utc->execute();
  ASSERT_TRUE(utc->ok()) << utc->errorMsg();
  EXPECT_EQ(1, utc->getMatchCount());

  auto denver = localReq(soluxNode->getSearchEngine());
  denver->collection("main").timeZone("America/Denver").topDocs("q")
      .matchQuery("when_dt", "2024-01-15").withStats();
  denver->execute();
  ASSERT_TRUE(denver->ok()) << denver->errorMsg();
  EXPECT_EQ(0, denver->getMatchCount());
}

TEST_F(DateFieldTest, skippedQueryGranuleWarnsAndInvalidZoneIsEager) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "apia", "when_dt", "2011-12-30T10:00:00Z"),
               UpdateMessage::COMMIT);  // 2011-12-31 local after the dateline jump

  auto skipped = localReq(soluxNode->getSearchEngine());
  skipped->collection("main").timeZone("Pacific/Apia").topDocs("q")
      .matchQuery("when_dt", "2011-12-30").withStats();
  skipped->execute();
  ASSERT_TRUE(skipped->ok()) << skipped->errorMsg();
  EXPECT_EQ(1, skipped->getMatchCount());
  EXPECT_TRUE(zoneRange("2011-12-30", testZone("Pacific/Apia")).granuleSkipped);
  EXPECT_TRUE(zoneRange("2011-12-30||+0DAY", testZone("Pacific/Apia")).granuleSkipped);
  ASSERT_TRUE(skipped->hasWarning("date_granule_skipped"));
  EXPECT_NE(skipped->respWarnings()[0].message.find("op 'q'"), std::string_view::npos);
  EXPECT_NE(skipped->respWarnings()[0].message.find("2011-12-30"), std::string_view::npos);
  EXPECT_NE(skipped->respWarnings()[0].message.find("Pacific/Apia"), std::string_view::npos);

  auto invalid = localReq(soluxNode->getSearchEngine());
  invalid->collection("main").timeZone("No/Such_Zone").topDocs("q").allQuery();
  invalid->execute();
  EXPECT_FALSE(invalid->ok());
  EXPECT_NE(invalid->errorMsg().find("No/Such_Zone"), std::string::npos);
  EXPECT_NE(invalid->errorMsg().find("tzdb version"), std::string::npos);
}

// Equality and range endpoints round by the literal's granularity end-to-end.
TEST_F(DateFieldTest, queryGranularity) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "d1", "when_dt", "2024-06-25T08:00:00Z"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "d2", "when_dt", "2024-06-25T18:30:00Z"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "d3", "when_dt", "2024-06-26T00:00:00Z"), UpdateMessage::COMMIT);

  auto count = [&](std::function<void(OpCursor&)> setQuery) {
    auto req = localReq(soluxNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    setQuery(cur);
    cur.withStats();
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getMatchCount();
  };

  // equality on a day matches the whole day, not the midnight instant
  EXPECT_EQ(2, count([](OpCursor& c) { c.matchQuery("when_dt", "2024-06-25"); }));
  EXPECT_EQ(1, count([](OpCursor& c) { c.matchQuery("when_dt", "2024-06-25T08:00:00Z"); }));
  EXPECT_EQ(3, count([](OpCursor& c) { c.matchQuery("when_dt", "2024-06"); }));
  EXPECT_EQ(2, count([](OpCursor& c) {
    c.exprQuery("when_dt:2024-06-25T12:00:00Z||/d");
  }));
  EXPECT_EQ(2, count([](OpCursor& c) {
    c.simpleQuery("when_dt:2024-06-25T12:00:00Z/DAY", {"id"});
  }));

  // range endpoints include the granule they name; exclusive excludes it whole
  auto range = [&](const char* gteV, const char* ltV, bool loIncl, bool hiIncl) {
    return count([&](OpCursor& c) {
      auto& r = c.rawQuery().kind.emplace<solux::api::RangeQuery>();
      auto& mr = c.mr();
      r.field = build::arenaStr(mr, "when_dt");
      auto* loVal = (solux::api::Val*)mr.allocate(sizeof(solux::api::Val), alignof(solux::api::Val));
      new (loVal) solux::api::Val();
      loVal->kind = build::arenaStr(mr, gteV);
      auto* hiVal = (solux::api::Val*)mr.allocate(sizeof(solux::api::Val), alignof(solux::api::Val));
      new (hiVal) solux::api::Val();
      hiVal->kind = build::arenaStr(mr, ltV);
      if (loIncl) r.gte = loVal; else r.gt = loVal;
      if (hiIncl) r.lte = hiVal; else r.lt = hiVal;
    });
  };
  EXPECT_EQ(3, range("2024-06-25", "2024-06-26", true, true));   // both days whole
  EXPECT_EQ(2, range("2024-06-25", "2024-06-26", true, false));  // lt excludes day 26
  EXPECT_EQ(1, range("2024-06-25", "2024-06-26", false, true));  // gt excludes day 25
}

// An unparseable date string fails just that doc (same contract as a bad
// vector value): the doc is reported in errors, peers index normally.
TEST_F(DateFieldTest, badDateMarksDocFailed) {
  CollectionHelper helper;

  std::vector<Doc> docs = {
    flatdoc("id", "g1", "when_dt", "2020-01-01T00:00:00Z"),
    flatdoc("id", "b1", "when_dt", "totally-not-a-date"),
    flatdoc("id", "g2", "when_dt", "2021-01-01T00:00:00Z"),
  };
  auto result = helper.indexAll(docs, UpdateMessage::COMMIT);

  ASSERT_EQ(solux::api::UpdateResponse_::Status::PARTIAL, result.status);
  ASSERT_EQ(1, result.errors.size());
  EXPECT_EQ("b1", result.errors[0].id);
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("when_dt"));

  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("main").topDocs("q").allQuery().fields({"id"}).limit(10);
  lreq->execute();
  std::vector<std::string> ids;
  for (auto& doc : lreq->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id")));
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ((std::vector<std::string>{"g1", "g2"}), ids);
}
