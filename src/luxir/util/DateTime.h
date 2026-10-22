// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "luxir/util/Clock.h"
#include "luxir/util/Cursor.h"
#include "luxir/util/log.h"

namespace luxir {

// Date/time helpers for the DATE field type.  Parsing, date math and civil
// arithmetic work in int64 milliseconds since the Unix epoch
// (1970-01-01T00:00:00Z), the convention Solr (DatePointField) and OpenSearch
// (date) use, with a sub-millisecond remainder carried beside it.  A field
// stores int64 units since the epoch at its DateUnit (millis or nanos);
// the conversion happens at the edges (toDateUnits,
// dateUnitWindow).  The value is a naturally-sorting signed integer, so it
// lives in the standard int column with identity encoding (no sortable-bits
// transform like FLOAT/DOUBLE).
//
// parseDateToEpochMillis accepts the union of OpenSearch's default
// "strict_date_optional_time || epoch_millis" (a superset of Solr's canonical
// 'YYYY-MM-DDThh:mm:ssZ'):
//   - an ISO-8601 calendar date 'YYYY-MM-DD' (or a month, 'YYYY-MM') with an
//     optional time ('Thh', 'Thh:mm', 'Thh:mm:ss', optional '.fff' fractional
//     seconds) and an optional 'Z' or +/-hh:mm zone offset (absent zone == UTC);
//   - a bare integer (optionally signed), interpreted as epoch milliseconds
//     (never as a bare year - the integer form keeps its meaning).
// It also accepts Solr and OpenSearch date math.  NOW is case-insensitive;
// fixed anchors may use Solr's direct suffix ('...Z+1DAY') or OpenSearch's
// separator ('...Z||+1d').  Commands chain left-to-right.  Solr word units
// are case-insensitive; OpenSearch's one-letter units are case-sensitive
// ('M' month, 'm' minute).  Returns nullopt if the text is not a recognized
// form.  Fractional seconds keep up to nanosecond precision (DateRange's
// loNanos/hiNanos); digits beyond nine are truncated.
//
// parseDateRange additionally reports the time WINDOW the text denotes at its
// own granularity: '2024-06-25' is the whole day, '2024-06' the whole month,
// '...T10:30' the whole minute.  Queries use the window (equality on a day
// matches the day; range endpoints include the granule they name); ingest and
// sorting use the window start (an instant).  A date-math anchor is an instant,
// even when its literal is partial; a /unit command creates the corresponding
// window so inclusive/exclusive range endpoints get OpenSearch-style rounding.

namespace datetime_detail {

// Supported instant range = the span representable by std::chrono::year, whose
// backing store is a 16-bit field ([-32767, 32767]).  Years outside this would
// silently narrow in the chrono::year constructor and slip past ymd.ok(), so we
// reject them. The matching epoch-millis bounds limit text parsing and civil
// arithmetic and ms ingest. Formatting rejects values
// outside this civil range before constructing a chrono::year.
inline constexpr int kMinYear = -32767;
inline constexpr int kMaxYear = 32767;
inline constexpr int64_t kMsPerDay = 86400000LL;
inline constexpr int kMaxLiteralOffsetMinutes = 23 * 60 + 59;
inline constexpr int kMaxZoneParamOffsetMinutes = 18 * 60;
inline constexpr int64_t kMinEpochMs =
    std::chrono::sys_days{std::chrono::year{kMinYear} / 1 / 1}.time_since_epoch().count() * kMsPerDay;
inline constexpr int64_t kMaxEpochMs =
    std::chrono::sys_days{std::chrono::year{kMaxYear} / 12 / 31}.time_since_epoch().count() * kMsPerDay
    + (kMsPerDay - 1);

// Hard caps bound work on untrusted text.  A literal is at most ~30 chars
// (9-digit year + full time + zone); math gets room for a useful command chain.
inline constexpr size_t kMaxLiteralParseLen = 64;
inline constexpr size_t kMaxDateMathParseLen = 256;

inline bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAsciiAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Date grammar helpers consume through the shared parser Cursor. Their only
// lookahead is Cursor::peekAt(), so the grammar contains no independent input
// pointer or array arithmetic.
inline bool fixedDigits(Cursor& cur, size_t count, int& out) {
  if (cur.remaining() < count) return false;
  int value = 0;
  for (size_t i = 0; i < count; ++i) {
    char ch = cur.peekAt(i);
    if (!isAsciiDigit(ch)) return false;
    value = value * 10 + (ch - '0');
  }
  cur.advance(count);
  out = value;
  return true;
}

// Consume 1..maxDigits leading digits into out; returns the count consumed
// (0 = no digit present). maxDigits keeps the accumulator from overflowing.
inline int varDigits(Cursor& cur, int maxDigits, long& out) {
  long value = 0;
  int count = 0;
  while (count < maxDigits && isAsciiDigit(cur.peek())) {
    value = value * 10 + (cur.peek() - '0');
    cur.advance();
    ++count;
  }
  out = value;
  return count;
}

struct IanaZoneRules;

} // namespace datetime_detail

// The UTC offset in force over the instants [begin, end), in epoch millis.
// Endpoints beyond the supported range clamp to [kMinEpochMs, kMaxEpochMs + 1].
struct ZoneInterval {
  int64_t begin;
  int64_t end;
  std::chrono::seconds offset;
};

// How a civil millisecond maps to instants. A SKIPPED civil time lies in a
// forward gap and a REPEATED one in a fold; before and after are the offsets
// on either side of that transition (equal for UNIQUE). The classification
// holds for every civil millisecond in [civilBegin, civilEnd), clamped like
// ZoneInterval: the whole gap or fold, or for a UNIQUE time the rest of its
// run up to the next gap or fold.
struct LocalOffsets {
  enum class Kind : uint8_t { UNIQUE, SKIPPED, REPEATED };
  Kind kind;
  std::chrono::seconds before;
  std::chrono::seconds after;
  int64_t civilBegin;
  int64_t civilEnd;
};

// A named IANA zone decoded from its compiled TZif file (DateTime.cpp). The
// process-lifetime registry owns every instance.
class IanaZone {
  std::string zoneName;
  std::unique_ptr<const datetime_detail::IanaZoneRules> rules;
  uint64_t id;  // never reused, unlike the address

public:
  IanaZone(std::string name, std::unique_ptr<const datetime_detail::IanaZoneRules> rules);
  ~IanaZone();

  std::string_view name() const { return zoneName; }
  std::chrono::seconds offsetAt(int64_t instant) const;
  ZoneInterval intervalAt(int64_t instant) const;
  LocalOffsets localAt(int64_t civilMillis) const;

private:
  std::runtime_error inconsistent(int64_t millis) const;
};

// A request's civil frame. UTC and fixed offsets use arithmetic-only fast
// paths; an IANA value points at a zone in the process-lifetime registry.
class TimeZone {
public:
  enum class Kind : uint8_t { UTC, FIXED, IANA };

private:
  Kind kind_;
  int offsetMinutes_;
  const IanaZone* zone_;

  constexpr TimeZone(Kind kind, int offsetMinutes, const IanaZone* zone)
    : kind_(kind), offsetMinutes_(offsetMinutes), zone_(zone) {}

public:
  static constexpr TimeZone utc() { return TimeZone(Kind::UTC, 0, nullptr); }
  static constexpr TimeZone fixed(int offsetMinutes) {
    return offsetMinutes == 0 ? utc() : TimeZone(Kind::FIXED, offsetMinutes, nullptr);
  }
  static constexpr TimeZone iana(const IanaZone* zone) {
    return TimeZone(Kind::IANA, 0, zone);
  }

  constexpr Kind kind() const { return kind_; }
  constexpr bool isUtc() const { return kind_ == Kind::UTC; }
  constexpr bool isFixed() const { return kind_ == Kind::FIXED; }
  constexpr bool isIana() const { return kind_ == Kind::IANA; }
  constexpr int offsetMinutes() const { return offsetMinutes_; }
  constexpr const IanaZone* ianaZone() const { return zone_; }

  std::string name() const {
    if (isUtc()) return "UTC";
    if (isIana()) return std::string(zone_->name());
    int magnitude = std::abs(offsetMinutes_);
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%c%02d:%02d",
                  offsetMinutes_ < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
    return buf;
  }

  friend constexpr bool operator==(const TimeZone&, const TimeZone&) = default;
};

namespace datetime_detail {

// The zones of one zoneinfo tree: every Zone and Link that its tzdata.zi
// names, decoded from the tree's compiled TZif files. The process loads one on
// first use (DateTime.cpp).
class ZoneRegistry {
  std::vector<std::unique_ptr<IanaZone>> zones;
  std::vector<std::pair<std::string, const IanaZone*>> names;  // sorted

public:
  std::string version = "unavailable";
  std::string error;

  // An unreadable tzdata.zi or a tree with no loadable zone sets error.
  static ZoneRegistry load(const std::string& root);
  // Exact, case-sensitive Zone or Link name; a link yields its target zone.
  const IanaZone* find(std::string_view name) const;
  size_t zoneCount() const { return zones.size(); }
  const IanaZone& zone(size_t index) const { return *zones[index]; }
};

// $TZDIR, else /usr/share/zoneinfo, read once.
const ZoneRegistry& zoneRegistry();

inline bool parseFixedZoneOffset(std::string_view spec, int& offsetMinutes) {
  if (spec.empty() || (spec[0] != '+' && spec[0] != '-')) return false;
  if (spec.size() != 3 && spec.size() != 5 && spec.size() != 6) return false;
  if (!isAsciiDigit(spec[1]) || !isAsciiDigit(spec[2])) return false;
  int hours = (spec[1] - '0') * 10 + (spec[2] - '0');
  int minutes = 0;
  if (spec.size() == 5) {
    if (!isAsciiDigit(spec[3]) || !isAsciiDigit(spec[4])) return false;
    minutes = (spec[3] - '0') * 10 + (spec[4] - '0');
  } else if (spec.size() == 6) {
    if (spec[3] != ':' || !isAsciiDigit(spec[4]) || !isAsciiDigit(spec[5])) return false;
    minutes = (spec[4] - '0') * 10 + (spec[5] - '0');
  }
  int magnitude = hours * 60 + minutes;
  if (minutes > 59 || magnitude > kMaxZoneParamOffsetMinutes) return false;
  offsetMinutes = spec[0] == '-' ? -magnitude : magnitude;
  return true;
}

} // namespace datetime_detail

// Force the process zone registry to load at server startup. Failure is
// recorded rather than thrown so UTC/fixed-only service remains available.
inline void preWarmTimeZoneDatabase() {
  datetime_detail::zoneRegistry();
}

inline bool timeZoneDatabaseAvailable() {
  return datetime_detail::zoneRegistry().error.empty();
}

inline std::string_view timeZoneDatabaseVersion() {
  return datetime_detail::zoneRegistry().version;
}

inline std::string_view timeZoneDatabaseError() {
  return datetime_detail::zoneRegistry().error;
}

// Exact request-zone grammar: UTC aliases, fixed +/-hh[[:]mm] capped at
// 18:00, then a case-sensitive IANA lookup. No trimming or custom aliases.
inline std::optional<TimeZone> resolveTimeZone(std::string_view spec) {
  if (spec.empty() || spec == "Z" || spec == "UTC") return TimeZone::utc();
  int offsetMinutes = 0;
  if (datetime_detail::parseFixedZoneOffset(spec, offsetMinutes)) {
    return TimeZone::fixed(offsetMinutes);
  }
  // A leading sign selected the fixed-offset grammar. Malformed fixed offsets
  // are not eligible to become surprising IANA names.
  if (spec[0] == '+' || spec[0] == '-') return std::nullopt;

  const IanaZone* zone = datetime_detail::zoneRegistry().find(spec);
  if (zone == nullptr) return std::nullopt;
  return TimeZone::iana(zone);
}

inline std::string timeZoneResolutionError(std::string_view spec) {
  if (!spec.empty() && spec[0] != '+' && spec[0] != '-'
      && !timeZoneDatabaseAvailable()) {
    return "time zone '" + std::string(spec)
        + "' requires the unavailable IANA time-zone database (tzdb version "
        + std::string(timeZoneDatabaseVersion()) + "): "
        + std::string(timeZoneDatabaseError());
  }
  return "invalid time zone '" + std::string(spec) + "' (tzdb version "
      + std::string(timeZoneDatabaseVersion()) + ")";
}

// The [start, end) window a date text denotes at its own granularity.  Each
// edge is epoch millis plus a sub-millisecond remainder in [0, 999999] ns,
// nonzero only for literals with more than three fractional digits.
struct DateRange {
  int64_t lo;
  int64_t hiExclusive;
  bool granuleSkipped = false;
  int32_t loNanos = 0;
  int32_t hiNanos = 0;
  // NOW-anchored with no rounding coarser than a millisecond: the window
  // moves with every request millisecond.
  bool unroundedNow = false;
};

// Unit of an epoch value. Stored NANOS values reserve the int64 extrema for
// saturated query bounds and exclusive fences (1677-09-21 to 2262-04-11).
enum class DateUnit : uint8_t { MILLIS, NANOS };

inline constexpr int64_t dateUnitNanos(DateUnit unit) {
  switch (unit) {
    case DateUnit::MILLIS: return 1000000LL;
    case DateUnit::NANOS: return 1LL;
  }
  throw std::invalid_argument("Unknown date unit");
}

inline constexpr std::string_view dateUnitName(DateUnit unit) {
  switch (unit) {
    case DateUnit::MILLIS: return "ms";
    case DateUnit::NANOS: return "ns";
  }
  throw std::invalid_argument("Unknown date unit");
}

namespace datetime_detail {

// Floor (or ceiling) of an instant, millis + nanos, in units of unitNanos.
// Exact in __int128 even when millis is an int64 extremum.
inline __int128 toUnits128(int64_t millis, int32_t nanos, int64_t unitNanos, bool roundUp) {
  __int128 total = (__int128)millis * 1000000 + nanos;
  __int128 quotient = total / unitNanos;
  __int128 remainder = total % unitNanos;
  if (remainder < 0 && !roundUp) --quotient;
  if (remainder > 0 && roundUp) ++quotient;
  return quotient;
}

inline std::optional<int64_t> epochInteger(std::string_view text) {
  if (text.empty() || text.size() > kMaxLiteralParseLen) return std::nullopt;
  if (text.starts_with('+')) {
    text.remove_prefix(1);
    if (text.empty() || !isAsciiDigit(text.front())) return std::nullopt;
  }
  int64_t value;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

} // namespace datetime_detail

inline int64_t clampDateUnits(__int128 value) {
  return (int64_t)std::clamp(value, (__int128)INT64_MIN, (__int128)INT64_MAX);
}

inline __int128 convertDateUnits(int64_t value, DateUnit from, DateUnit to) {
  __int128 nanos = (__int128)value * dateUnitNanos(from);
  int64_t divisor = dateUnitNanos(to);
  return nanos / divisor - (nanos % divisor < 0);
}

inline int64_t dateEpochMillis(int64_t value, DateUnit unit) {
  return (int64_t)convertDateUnits(value, unit, DateUnit::MILLIS);
}

// A stored instant in the given unit, rounded down (or up). Finer digits are
// truncated toward earlier time. NANOS excludes both int64 extrema; bounds
// use dateUnitWindow instead. nullopt when outside the stored domain.
inline std::optional<int64_t> toDateUnits(int64_t millis, int32_t nanos,
                                          DateUnit unit,
                                          bool roundUp = false) {
  __int128 units = datetime_detail::toUnits128(
      millis, nanos, dateUnitNanos(unit), roundUp);
  int64_t min = std::numeric_limits<int64_t>::min();
  int64_t max = std::numeric_limits<int64_t>::max();
  if (unit == DateUnit::NANOS) { ++min; --max; }
  if (units < min || units > max) {
    return std::nullopt;
  }
  return (int64_t)units;
}

// A query window in the field's units: [floor(lo), ceil(hiExclusive)), so a
// stored value matches when the instants it truncates from overlap the
// window (a sub-millisecond literal on an ms field matches its millisecond).
// Edges outside int64 saturate, so a bound past the NANOS range keeps its
// meaning (everything after it, or nothing before it).
inline std::pair<int64_t, int64_t> dateUnitWindow(const DateRange& range,
                                                  DateUnit unit) {
  namespace dd = datetime_detail;
  int64_t unitNanos = dateUnitNanos(unit);
  return {clampDateUnits(dd::toUnits128(range.lo, range.loNanos, unitNanos, false)),
          clampDateUnits(dd::toUnits128(range.hiExclusive, range.hiNanos, unitNanos, true))};
}

// A parsed literal additionally preserves whether an offset token was present.
// That bit distinguishes an offset-less civil literal from Z/+00:00 in a
// non-UTC request frame.
struct DateLiteralRange {
  DateRange range;
  bool hasExplicitOffset;
};

// Nominal local milliseconds since 1970 plus the actual offset at the source
// instant. The offset uses seconds because historical IANA rules are not
// restricted to whole minutes.
struct CivilTime {
  int64_t millis;
  std::chrono::seconds offset;
};

enum class CalendarUnit : uint8_t { DAY, WEEK, MONTH, QUARTER, YEAR };

inline std::optional<CivilTime> civilFromInstant(int64_t millis, const TimeZone& zone) {
  namespace dd = datetime_detail;
  using namespace std::chrono;
  if (millis < dd::kMinEpochMs || millis > dd::kMaxEpochMs) return std::nullopt;

  seconds offset{0};
  if (zone.isFixed()) {
    offset = minutes{zone.offsetMinutes()};
  } else if (zone.isIana()) {
    offset = zone.ianaZone()->offsetAt(millis);
  }

  __int128 local = (__int128)millis + duration_cast<milliseconds>(offset).count();
  // This numerical guard precedes every year_month_day construction by callers
  // and prevents chrono::year's 16-bit representation from narrowing.
  if (local < dd::kMinEpochMs || local > dd::kMaxEpochMs) return std::nullopt;
  return CivilTime{(int64_t)local, offset};
}

namespace datetime_detail {

// Java ZonedDateTime.ofLocal: a gap shifts forward by its length (the
// pre-transition offset); a fold retains a valid preferred source offset and
// otherwise uses the earlier (pre-transition) one.
inline std::chrono::seconds offsetForLocal(
    const LocalOffsets& local, std::optional<std::chrono::seconds> preferredOffset) {
  if (local.kind == LocalOffsets::Kind::REPEATED && preferredOffset == local.after) {
    return local.after;
  }
  return local.before;
}

inline std::optional<int64_t> instantWithOffset(
    int64_t civilMillis, std::chrono::seconds offset) {
  using namespace std::chrono;
  __int128 instant = (__int128)civilMillis
                   - duration_cast<milliseconds>(offset).count();
  if (instant < kMinEpochMs || instant > kMaxEpochMs) return std::nullopt;
  return (int64_t)instant;
}

} // namespace datetime_detail

inline std::optional<int64_t> instantFromCivil(
    int64_t civilMillis, const TimeZone& zone,
    std::optional<std::chrono::seconds> preferredOffset = std::nullopt) {
  namespace dd = datetime_detail;
  using namespace std::chrono;
  if (civilMillis < dd::kMinEpochMs || civilMillis > dd::kMaxEpochMs) {
    return std::nullopt;
  }

  seconds offset{0};
  if (zone.isFixed()) {
    offset = minutes{zone.offsetMinutes()};
  } else if (zone.isIana()) {
    offset = dd::offsetForLocal(zone.ianaZone()->localAt(civilMillis), preferredOffset);
  }
  return dd::instantWithOffset(civilMillis, offset);
}

// True when no instant has a civil time in [civilLo, civilNext): the whole
// civil interval lies inside one forward gap.
inline bool civilIntervalSkipped(
    int64_t civilLo, int64_t civilNext, const TimeZone& zone) {
  if (!zone.isIana() || civilNext <= civilLo) return false;
  auto local = zone.ianaZone()->localAt(civilLo);
  return local.kind == LocalOffsets::Kind::SKIPPED && civilNext <= local.civilEnd;
}

namespace datetime_detail {

// Local resolver for dense, mostly increasing civil fence sequences. The
// cached classification holds over its civil span, so a sequence that moves
// forward within one transition window resolves without another zone lookup.
class LocalTimeResolver {
  TimeZone zone;
  std::optional<std::chrono::seconds> preferredOffset;
  LocalOffsets cachedLocal{};
  bool cached = false;

public:
  LocalTimeResolver(const TimeZone& zone,
                    std::optional<std::chrono::seconds> preferredOffset)
    : zone(zone), preferredOffset(preferredOffset) {}

  std::optional<int64_t> resolve(int64_t civilMillis) {
    if (!zone.isIana()) {
      return instantFromCivil(civilMillis, zone, preferredOffset);
    }
    if (civilMillis < kMinEpochMs || civilMillis > kMaxEpochMs) return std::nullopt;
    if (!cached || civilMillis < cachedLocal.civilBegin
        || civilMillis >= cachedLocal.civilEnd) {
      cachedLocal = zone.ianaZone()->localAt(civilMillis);
      cached = true;
    }
    return instantWithOffset(civilMillis, offsetForLocal(cachedLocal, preferredOffset));
  }
};

} // namespace datetime_detail

// Parse only the literal forms above. Date-math splitting calls this for each
// possible fixed anchor, so it deliberately requires the whole input.
inline std::optional<DateLiteralRange> parseDateLiteralRange(
    std::string_view s, const TimeZone& frameZone, DateUnit numericUnit = DateUnit::MILLIS) {
  using namespace std::chrono;
  namespace dd = datetime_detail;
  Cursor input{s};
  if (input.atEnd() || input.size() > dd::kMaxLiteralParseLen) return std::nullopt;

  // Bare integer => epoch units. A real ISO date
  // always carries a '-' between year and month, so an all-digit run (after an
  // optional leading sign) is unambiguous.  Detected with range-checked
  // algorithms; converted with from_chars, which reports overflow as an error
  // rather than wrapping (signed-overflow UB).
  if (auto integer = dd::epochInteger(s)) {
    int64_t v = *integer;
    if (numericUnit == DateUnit::NANOS) {
      int64_t millis = dateEpochMillis(v, numericUnit);
      int32_t nanos = (int32_t)((__int128)v - (__int128)millis * 1000000);
      DateRange range{millis, millis + (nanos + 1) / 1000000};
      range.loNanos = nanos;
      range.hiNanos = (nanos + 1) % 1000000;
      return DateLiteralRange{range, false};
    }
    if (v < dd::kMinEpochMs || v > dd::kMaxEpochMs) return std::nullopt;
    return DateLiteralRange{DateRange{v, v + 1}, false};
  }

  Cursor c = input;
  int sign = 1;
  if (c.consume('-')) sign = -1;
  else c.consume('+');

  // Variable-width year, then '-MM' and an optional '-DD'.
  long year = 0;
  if (dd::varDigits(c, 9, year) == 0) return std::nullopt;
  year *= sign;
  int mo = 0, day = 1;
  if (!c.consume('-') || !dd::fixedDigits(c, 2, mo)) return std::nullopt;
  bool haveDay = false;
  if (c.consume('-')) {
    if (!dd::fixedDigits(c, 2, day)) return std::nullopt;
    haveDay = true;
  }

  int hh = 0, mm = 0, ss = 0, frac = 0, offsetMin = 0;
  // Fractional digits past the millisecond, as nanos, and the sub-millisecond
  // granule they pin down (0 = the literal stops at millis or coarser).
  int32_t subNanos = 0;
  int32_t granuleNanos = 0;
  bool hasExplicitOffset = false;
  // The granule width the text pins down, as fixed millis; 0 = a calendar
  // month (variable width, handled by date arithmetic below).
  int64_t granuleMs = haveDay ? dd::kMsPerDay : 0;

  // Optional time: 'T'/'t'/' ' then hh[:mm[:ss[.fff]]] (a time needs a day).
  if (!c.atEnd() && haveDay) {
    char t = c.peek();
    if (t != 'T' && t != 't' && t != ' ') return std::nullopt;  // junk after date
    c.advance();
    if (!dd::fixedDigits(c, 2, hh)) return std::nullopt;
    granuleMs = 3600000LL;
    if (c.consume(':')) {
      if (!dd::fixedDigits(c, 2, mm)) return std::nullopt;
      granuleMs = 60000LL;
      if (c.consume(':')) {
        if (!dd::fixedDigits(c, 2, ss)) return std::nullopt;
        granuleMs = 1000LL;
        char dot = c.peek();
        if (dot == '.' || dot == ',') {
          c.advance();
          if (!dd::isAsciiDigit(c.peek())) return std::nullopt;  // need >= 1 digit
          granuleMs = 1;
          // First three fractional digits -> millis, the next six -> nanos;
          // the rest are truncated.
          int digits = 0;
          int32_t scale = 100000000;
          while (dd::isAsciiDigit(c.peek())) {
            if (digits < 9) {
              int32_t digit = (c.peek() - '0') * scale;
              if (digits < 3) frac += digit / 1000000;
              else subNanos += digit;
              scale /= 10;
            }
            ++digits;
            c.advance();
          }
          if (digits > 3) granuleNanos = digits >= 9 ? 1 : scale * 10;
        }
      }
    }
    // Optional zone: 'Z'/'z' or +/-hh[:mm].
    if (!c.atEnd()) {
      char z = c.peek();
      if (z == 'Z' || z == 'z') {
        hasExplicitOffset = true;
        c.advance();
      } else if (z == '+' || z == '-') {
        hasExplicitOffset = true;
        int zsign = (z == '-') ? -1 : 1;
        c.advance();
        int zh = 0, zm = 0;
        if (!dd::fixedDigits(c, 2, zh)) return std::nullopt;
        c.consume(':');  // optional separator ('hh:mm' or 'hhmm')
        if (dd::isAsciiDigit(c.peek())) {
          if (!dd::fixedDigits(c, 2, zm)) return std::nullopt;
        }
        if (zm > 59 || zh * 60 + zm > dd::kMaxLiteralOffsetMinutes) {
          return std::nullopt;
        }
        offsetMin = zsign * (zh * 60 + zm);
      } else {
        return std::nullopt;
      }
    }
  }
  if (!c.atEnd()) return std::nullopt;  // any leftover (incl. embedded NUL + junk) is invalid

  if (mo < 1 || mo > 12 || day < 1 || day > 31) return std::nullopt;
  if (hh > 23 || mm > 59 || ss > 59) return std::nullopt;
  // Guard the year before constructing chrono::year: its 16-bit store would
  // silently narrow an out-of-range year into a valid-looking one that ok()
  // then accepts.  The bound also keeps the millis arithmetic below well clear
  // of int64 overflow (max |epochDays| ~ 1.2e7, * 86.4e6 ~ 1e15 << INT64_MAX).
  if (year < dd::kMinYear || year > dd::kMaxYear) return std::nullopt;
  year_month_day ymd{std::chrono::year{(int)year},
                     std::chrono::month{(unsigned)mo},
                     std::chrono::day{(unsigned)day}};
  if (!ymd.ok()) return std::nullopt;  // rejects e.g. Feb 30, non-leap Feb 29

  int64_t epochDays = sys_days{ymd}.time_since_epoch().count();
  int64_t civilLo = epochDays * dd::kMsPerDay
                  + (int64_t)hh * 3600000LL + (int64_t)mm * 60000LL
                  + (int64_t)ss * 1000LL + frac;

  int64_t civilLast;
  std::optional<int64_t> civilNext;
  if (granuleMs != 0) {
    civilLast = std::min(civilLo + granuleMs - 1, dd::kMaxEpochMs);
    if (civilLast < dd::kMaxEpochMs) civilNext = civilLast + 1;
  } else if (mo == 12 && year == dd::kMaxYear) {
    // Do not construct chrono::year{kMaxYear + 1}: it narrows before it can
    // be inspected.
    civilLast = dd::kMaxEpochMs;
  } else {
    year_month_day next = (mo == 12)
        ? year_month_day{std::chrono::year{(int)year + 1}, std::chrono::month{1},
                         std::chrono::day{1}}
        : year_month_day{ymd.year(), std::chrono::month{(unsigned)mo + 1},
                         std::chrono::day{1}};
    civilNext = sys_days{next}.time_since_epoch().count() * dd::kMsPerDay;
    civilLast = *civilNext - 1;
  }

  TimeZone literalZone = hasExplicitOffset ? TimeZone::fixed(offsetMin) : frameZone;
  auto lo = instantFromCivil(civilLo, literalZone);
  if (!lo) return std::nullopt;
  auto source = civilFromInstant(*lo, literalZone);
  if (!source) return std::nullopt;
  auto last = instantFromCivil(civilLast, literalZone, source->offset);
  if (!last || *last < *lo) return std::nullopt;

  bool granuleSkipped = !hasExplicitOffset && civilNext
      && civilIntervalSkipped(civilLo, *civilNext, frameZone);
  DateRange range{*lo, *last + 1, granuleSkipped};
  if (granuleNanos != 0) {
    // A sub-millisecond granule sits inside the one millisecond at *lo.
    int32_t hiNanos = subNanos + granuleNanos;
    range.loNanos = subNanos;
    range.hiExclusive = *lo + hiNanos / 1000000;
    range.hiNanos = hiNanos % 1000000;
  }
  return DateLiteralRange{range, hasExplicitOffset};
}

namespace datetime_detail {

enum class DateMathUnit : uint8_t {
  YEAR,
  MONTH,
  WEEK,
  DAY,
  HOUR,
  MINUTE,
  SECOND,
  MILLISECOND
};

inline bool asciiEqualIgnoreCase(std::string_view a, std::string_view b) {
  Cursor input{a};
  return input.size() == b.size() && input.startsWithAsciiIgnoreCase(b);
}

// OpenSearch abbreviations are intentionally case-sensitive: M is month and
// m is minute. Solr's spelled-out aliases are case-insensitive.
inline std::optional<DateMathUnit> parseDateMathUnit(std::string_view unit) {
  if (unit == "y") return DateMathUnit::YEAR;
  if (unit == "M") return DateMathUnit::MONTH;
  if (unit == "w") return DateMathUnit::WEEK;
  if (unit == "d") return DateMathUnit::DAY;
  if (unit == "h" || unit == "H") return DateMathUnit::HOUR;
  if (unit == "m") return DateMathUnit::MINUTE;
  if (unit == "s") return DateMathUnit::SECOND;

  auto is = [&](std::string_view name) { return asciiEqualIgnoreCase(unit, name); };
  if (is("YEAR") || is("YEARS")) return DateMathUnit::YEAR;
  if (is("MONTH") || is("MONTHS")) return DateMathUnit::MONTH;
  if (is("WEEK") || is("WEEKS")) return DateMathUnit::WEEK;
  if (is("DAY") || is("DAYS") || is("DATE")) return DateMathUnit::DAY;
  if (is("HOUR") || is("HOURS")) return DateMathUnit::HOUR;
  if (is("MINUTE") || is("MINUTES")) return DateMathUnit::MINUTE;
  if (is("SECOND") || is("SECONDS")) return DateMathUnit::SECOND;
  if (is("MILLI") || is("MILLIS") || is("MILLISECOND") || is("MILLISECONDS")) {
    return DateMathUnit::MILLISECOND;
  }
  return std::nullopt;
}

inline bool supportedInstant(int64_t millis) {
  return millis >= kMinEpochMs && millis <= kMaxEpochMs;
}

inline std::optional<int64_t> addFixed(int64_t millis, int64_t amount, int64_t unitMillis) {
  __int128 result = (__int128)millis + (__int128)amount * unitMillis;
  if (result < kMinEpochMs || result > kMaxEpochMs) return std::nullopt;
  return (int64_t)result;
}

// Calendar addition with end-of-month clamping (Jan 31 + 1 month is the last
// day of February), computed from the supplied civil start.
inline std::optional<int64_t> calendarCivil(
    int64_t civilMillis, int64_t amount, DateMathUnit unit) {
  using namespace std::chrono;
  local_time<milliseconds> tp{milliseconds{civilMillis}};
  local_days dp = floor<days>(tp);
  milliseconds timeOfDay = tp - dp;
  year_month_day ymd{dp};

  __int128 targetYear = (int)ymd.year();
  int targetMonth = (int)(unsigned)ymd.month();
  if (unit == DateMathUnit::YEAR) {
    targetYear += amount;
  } else {
    __int128 totalMonths = targetYear * 12 + (targetMonth - 1) + amount;
    // Floor division, not C++ truncation, for negative proleptic years.
    targetYear = totalMonths / 12;
    __int128 remainder = totalMonths % 12;
    if (remainder < 0) {
      remainder += 12;
      --targetYear;
    }
    targetMonth = (int)remainder + 1;
  }
  if (targetYear < kMinYear || targetYear > kMaxYear) return std::nullopt;

  year y{(int)targetYear};
  month m{(unsigned)targetMonth};
  unsigned lastDay = (unsigned)year_month_day_last{y, month_day_last{m}}.day();
  unsigned targetDay = std::min((unsigned)ymd.day(), lastDay);
  int64_t outCivil = local_days{year_month_day{y, m, day{targetDay}}}
                       .time_since_epoch().count() * kMsPerDay
                   + timeOfDay.count();
  return outCivil;
}

// Calendar addition on an instant. The source instant's offset is the fold
// preference at the target.
inline std::optional<int64_t> addCalendar(int64_t millis, int64_t amount,
                                          DateMathUnit unit, const TimeZone& zone) {
  auto source = civilFromInstant(millis, zone);
  if (!source) return std::nullopt;
  auto outCivil = calendarCivil(source->millis, amount, unit);
  if (!outCivil) return std::nullopt;
  return instantFromCivil(*outCivil, zone, source->offset);
}

inline std::optional<int64_t> addUnit(int64_t millis, int64_t amount,
                                      DateMathUnit unit, const TimeZone& zone) {
  switch (unit) {
    case DateMathUnit::YEAR:
    case DateMathUnit::MONTH:
      return addCalendar(millis, amount, unit, zone);
    case DateMathUnit::WEEK:
    case DateMathUnit::DAY:
      if (!zone.isIana()) {
        return addFixed(millis, amount,
                        unit == DateMathUnit::WEEK ? 7 * kMsPerDay : kMsPerDay);
      } else {
        auto source = civilFromInstant(millis, zone);
        if (!source) return std::nullopt;
        int64_t daysPerUnit = unit == DateMathUnit::WEEK ? 7 : 1;
        __int128 civil = (__int128)source->millis
                       + (__int128)amount * daysPerUnit * kMsPerDay;
        if (civil < kMinEpochMs || civil > kMaxEpochMs) return std::nullopt;
        return instantFromCivil((int64_t)civil, zone, source->offset);
      }
    case DateMathUnit::HOUR: return addFixed(millis, amount, 3600000LL);
    case DateMathUnit::MINUTE: return addFixed(millis, amount, 60000LL);
    case DateMathUnit::SECOND: return addFixed(millis, amount, 1000LL);
    case DateMathUnit::MILLISECOND: return addFixed(millis, amount, 1);
  }
  return std::nullopt;
}

// Return the inclusive [start,end] interval containing millis at unit
// granularity in the civil frame. Week follows ISO/OpenSearch and starts
// Monday. The upper edge resolves the last inclusive civil millisecond, not
// the next granule's start.
inline std::optional<std::pair<int64_t, int64_t>> roundedInterval(
    int64_t millis, DateMathUnit unit, const TimeZone& zone) {
  using namespace std::chrono;
  if (unit == DateMathUnit::MILLISECOND) return std::pair{millis, millis};

  auto source = civilFromInstant(millis, zone);
  if (!source) return std::nullopt;
  local_time<milliseconds> tp{milliseconds{source->millis}};
  int64_t civilLo;
  int64_t civilLast;

  if (unit == DateMathUnit::SECOND) {
    civilLo = floor<seconds>(tp).time_since_epoch().count() * 1000LL;
    civilLast = civilLo + 999;
  } else if (unit == DateMathUnit::MINUTE) {
    civilLo = floor<minutes>(tp).time_since_epoch().count() * 60000LL;
    civilLast = civilLo + 59999;
  } else if (unit == DateMathUnit::HOUR) {
    civilLo = floor<hours>(tp).time_since_epoch().count() * 3600000LL;
    civilLast = civilLo + 3599999;
  } else {
    local_days dp = floor<days>(tp);
    if (unit == DateMathUnit::DAY) {
      civilLo = dp.time_since_epoch().count() * kMsPerDay;
      civilLast = civilLo + kMsPerDay - 1;
    } else if (unit == DateMathUnit::WEEK) {
      unsigned isoDay = weekday{dp}.iso_encoding();  // Monday=1 ... Sunday=7
      local_days start = dp - days{isoDay - 1};
      civilLo = start.time_since_epoch().count() * kMsPerDay;
      civilLast = civilLo + 7 * kMsPerDay - 1;
    } else {
      year_month_day ymd{dp};
      year y = ymd.year();
      month m = unit == DateMathUnit::YEAR ? month{1} : ymd.month();
      civilLo = local_days{year_month_day{y, m, day{1}}}
                    .time_since_epoch().count() * kMsPerDay;
      if (unit == DateMathUnit::YEAR) {
        int64_t civilHi = (int)y == kMaxYear
            ? kMaxEpochMs + 1
            : local_days{year_month_day{y + years{1}, month{1}, day{1}}}
                  .time_since_epoch().count() * kMsPerDay;
        civilLast = civilHi - 1;
      } else if ((int)y == kMaxYear && (unsigned)m == 12) {
        civilLast = kMaxEpochMs;
      } else {
        year_month ym = y / m + months{1};
        int64_t civilHi = local_days{year_month_day{ym.year(), ym.month(), day{1}}}
                            .time_since_epoch().count() * kMsPerDay;
        civilLast = civilHi - 1;
      }
    }
  }

  if (civilLo < kMinEpochMs || civilLo > kMaxEpochMs
      || civilLast < kMinEpochMs || civilLast > kMaxEpochMs) {
    return std::nullopt;
  }
  auto lo = instantFromCivil(civilLo, zone, source->offset);
  auto last = instantFromCivil(civilLast, zone, source->offset);
  if (!lo || !last || *last < *lo) return std::nullopt;
  return std::pair{*lo, *last};
}

struct DateMathResult {
  DateRange range;
  bool rounded;
};

// Evaluate once for the lower edge and once for the upper edge. A slash floors
// the lower edge and rounds the upper edge to the unit's last millisecond;
// later commands transform both. This is exactly the interval the existing
// DATE range fold needs for gte/gt/lte/lt semantics. rounded is true when some
// /unit coarser than a millisecond was applied.  The edges carry the
// anchor's sub-millisecond part (loNanos, and hiLastNanos for the last
// inclusive nano of the high edge's millisecond) through additions, which are
// whole milliseconds; rounding to any unit clears it.
inline std::optional<DateMathResult> applyDateMath(
    int64_t anchor, std::string_view math, const TimeZone& zone,
    int32_t loNanos = 0, int32_t hiLastNanos = 999999) {
  Cursor cur{math};
  if (cur.atEnd()) return std::nullopt;
  int64_t lo = anchor;
  int64_t hi = anchor;
  bool rounded = false;
  while (!cur.atEnd()) {
    char op = cur.peek();
    cur.advance();
    if (op != '+' && op != '-' && op != '/') return std::nullopt;

    int64_t amount = 0;
    if (op != '/') {
      std::string_view amountText = cur.takeWhile(isAsciiDigit);
      if (amountText.empty()) return std::nullopt;
      auto [ptr, ec] = std::from_chars(amountText.begin(), amountText.end(), amount);
      if (ec != std::errc{} || ptr != amountText.end()) return std::nullopt;
      if (op == '-') amount = -amount;
    }

    std::string_view unitText = cur.takeWhile(isAsciiAlpha);
    if (unitText.empty()) return std::nullopt;
    auto unit = parseDateMathUnit(unitText);
    if (!unit) return std::nullopt;

    if (op == '/') {
      auto lowWindow = roundedInterval(lo, *unit, zone);
      auto highWindow = roundedInterval(hi, *unit, zone);
      if (!lowWindow || !highWindow) return std::nullopt;
      lo = lowWindow->first;
      hi = highWindow->second;
      loNanos = 0;
      hiLastNanos = 999999;
      rounded |= *unit != DateMathUnit::MILLISECOND;
    } else {
      auto newLo = addUnit(lo, amount, *unit, zone);
      auto newHi = addUnit(hi, amount, *unit, zone);
      if (!newLo || !newHi || *newLo > *newHi) return std::nullopt;
      lo = *newLo;
      hi = *newHi;
    }
  }
  if (!supportedInstant(lo) || !supportedInstant(hi) || lo > hi) return std::nullopt;
  DateRange range{lo, hi + 1};
  range.loNanos = loNanos;
  if (hiLastNanos != 999999) {
    range.hiExclusive = hi;
    range.hiNanos = hiLastNanos + 1;
  }
  return DateMathResult{.range = range, .rounded = rounded};
}

// The math edges for a literal anchor: its start, and the last nano of its
// first millisecond, or of its sub-millisecond granule.
inline std::pair<int32_t, int32_t> anchorNanos(const DateRange& anchor) {
  if (anchor.loNanos == 0 && anchor.hiNanos == 0) return {0, 999999};
  return {anchor.loNanos,
          (int32_t)((anchor.hiExclusive - anchor.lo) * 1000000 + anchor.hiNanos - 1)};
}

inline bool startsWithNow(const Cursor& input) {
  return input.startsWithAsciiIgnoreCase("NOW");
}

} // namespace datetime_detail

// Pure civil arithmetic, always from the original anchor. Month-end clamping
// must not accumulate (Jan 31 -> Feb 29 -> Mar 31).
inline std::optional<int64_t> civilCalendarAt(
    int64_t startCivil, int64_t amount, CalendarUnit unit) {
  namespace dd = datetime_detail;
  if (unit == CalendarUnit::DAY || unit == CalendarUnit::WEEK) {
    int64_t daysPerUnit = unit == CalendarUnit::WEEK ? 7 : 1;
    __int128 target = (__int128)startCivil
                    + (__int128)amount * daysPerUnit * dd::kMsPerDay;
    if (target < dd::kMinEpochMs || target > dd::kMaxEpochMs) return std::nullopt;
    return (int64_t)target;
  }
  if (unit == CalendarUnit::YEAR) {
    return dd::calendarCivil(startCivil, amount, dd::DateMathUnit::YEAR);
  }
  __int128 months = (__int128)amount * (unit == CalendarUnit::QUARTER ? 3 : 1);
  if (months < std::numeric_limits<int64_t>::min()
      || months > std::numeric_limits<int64_t>::max()) return std::nullopt;
  return dd::calendarCivil(startCivil, (int64_t)months, dd::DateMathUnit::MONTH);
}

inline std::optional<DateRange> parseDateRange(std::string_view s, int64_t nowEpochMillis,
                                               const TimeZone& zone,
                                               DateUnit numericUnit = DateUnit::MILLIS) {
  namespace dd = datetime_detail;
  Cursor input{s};
  if (input.atEnd() || input.size() > dd::kMaxDateMathParseLen
      || !dd::supportedInstant(nowEpochMillis)) {
    return std::nullopt;
  }

  // NOW/now is the one non-literal anchor. Case-insensitive acceptance lets
  // Solr and OpenSearch clients share the same endpoint.
  if (dd::startsWithNow(input)) {
    if (input.size() == 3) {
      return DateRange{.lo = nowEpochMillis, .hiExclusive = nowEpochMillis + 1,
                       .unroundedNow = true};
    }
    input.advance(3);
    input.consume("||");  // harmless union convenience
    auto result = dd::applyDateMath(
        nowEpochMillis, input.slice(input.position(), input.size()), zone);
    if (!result) return std::nullopt;
    result->range.unroundedNow = !result->rounded;
    return result->range;
  }

  // A literal without math retains Luxir's existing granularity window.
  if (auto literal = parseDateLiteralRange(input.slice(0, input.size()), zone, numericUnit)) {
    return literal->range;
  }

  // OpenSearch uses an explicit anchor/math separator. It removes every
  // ambiguity with a partial time or numeric epoch anchor.
  Cursor separatorScan = input;
  if (separatorScan.findNext("||")) {
    size_t separator = separatorScan.position();
    separatorScan.advance(2);
    size_t mathStart = separatorScan.position();
    if (separatorScan.findNext("||")) return std::nullopt;
    auto anchor = parseDateLiteralRange(input.slice(0, separator), zone, numericUnit);
    if (!anchor) return std::nullopt;
    auto [loNanos, hiLastNanos] = dd::anchorNanos(anchor->range);
    auto result = dd::applyDateMath(
        anchor->range.lo, input.slice(mathStart, input.size()), zone,
        loNanos, hiLastNanos);
    if (!result) return std::nullopt;
    result->range.granuleSkipped = anchor->range.granuleSkipped;
    return result->range;
  }

  // Solr appends math directly. Try operator positions from right to left so
  // '-' in the calendar date and +/- in a zone offset remain part of the
  // longest valid anchor. Both the anchor and the command suffix must parse.
  Cursor suffixScan = input;
  suffixScan.seek(suffixScan.size());
  while (suffixScan.position() > 1) {
    suffixScan.retreat();
    char c = suffixScan.peek();
    if (c != '+' && c != '-' && c != '/') continue;
    size_t separator = suffixScan.position();
    auto anchor = parseDateLiteralRange(input.slice(0, separator), zone, numericUnit);
    if (!anchor) continue;
    auto [loNanos, hiLastNanos] = dd::anchorNanos(anchor->range);
    if (auto result = dd::applyDateMath(
            anchor->range.lo, input.slice(separator, input.size()), zone,
            loNanos, hiLastNanos)) {
      result->range.granuleSkipped = anchor->range.granuleSkipped;
      return result->range;
    }
  }
  return std::nullopt;
}

inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s,
                                                      int64_t nowEpochMillis,
                                                      const TimeZone& zone) {
  auto r = parseDateRange(s, nowEpochMillis, zone);
  if (!r) return std::nullopt;
  return toDateUnits(r->lo, r->loNanos, DateUnit::MILLIS);
}

// Ingest has no request time-zone surface and intentionally remains UTC.
inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s,
                                                      int64_t nowEpochMillis) {
  return parseDateToEpochMillis(s, nowEpochMillis, TimeZone::utc());
}

inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s) {
  return parseDateToEpochMillis(s, currentEpochMillis(), TimeZone::utc());
}

// Render an epoch value in UTC, with 3 fraction digits for ms and the fewest
// of 3/6/9 that is exact for ns. Bound sentinels can be formatted too.
inline std::string formatDateIso8601(int64_t value, DateUnit dateUnit) {
  using namespace std::chrono;
  namespace dd = datetime_detail;
  int64_t unit = dateUnitNanos(dateUnit);
  int64_t unitsPerSecond = 1000000000LL / unit;
  int64_t secs = value / unitsPerSecond;
  int64_t rem = value % unitsPerSecond;
  if (rem < 0) {
    rem += unitsPerSecond;
    --secs;
  }
  int64_t nanos = rem * unit;
  if (secs < dd::kMinEpochMs / 1000 || secs > dd::kMaxEpochMs / 1000) {
    throw std::out_of_range("Date is outside the supported ISO-8601 year range");
  }
  sys_seconds tp{seconds{secs}};
  sys_days dp = floor<days>(tp);
  year_month_day ymd{dp};
  hh_mm_ss hms{tp - dp};
  int y = (int)ymd.year();
  unsigned mo = (unsigned)ymd.month();
  unsigned d = (unsigned)ymd.day();
  int hh = (int)hms.hours().count();
  int mm = (int)hms.minutes().count();
  int ss = (int)hms.seconds().count();
  char buf[48];
  int len = std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d", y, mo, d, hh, mm, ss);
  int digits = nanos % 1000000 == 0 ? 3 : nanos % 1000 == 0 ? 6 : 9;
  int64_t fraction = digits == 3 ? nanos / 1000000 : digits == 6 ? nanos / 1000 : nanos;
  len += std::snprintf(buf + len, sizeof buf - (size_t)len, ".%0*lld", digits, (long long)fraction);
  buf[len++] = 'Z';
  return std::string(buf, (size_t)len);
}

inline std::string formatEpochMillisIso8601(int64_t millis) {
  return formatDateIso8601(millis, DateUnit::MILLIS);
}

} // namespace luxir
