#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "solux/util/Clock.h"
#include "solux/util/Cursor.h"
#include "solux/util/log.h"

namespace solux {

// Date/time helpers for the DATE field type.  Dates are stored internally as
// int64 milliseconds since the Unix epoch (1970-01-01T00:00:00Z), the same
// convention Solr (DatePointField) and OpenSearch (date) use.  Millis is a
// naturally-sorting signed integer, so the value lives in the standard int
// column with identity encoding (no sortable-bits transform like FLOAT/DOUBLE).
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
// form.  Fractional seconds beyond millisecond precision are truncated.
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
// reject them; the matching epoch-millis bounds clamp the bare-integer path so
// every stored value also formats back safely.  The range is absurdly wide for
// real timestamps (year +/-32767), so this rejects only garbage.
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

} // namespace datetime_detail

// A request's civil frame. UTC and fixed offsets use arithmetic-only fast
// paths; an IANA value points into the process-lifetime chrono tzdb snapshot.
class TimeZone {
public:
  enum class Kind : uint8_t { UTC, FIXED, IANA };

private:
  Kind kind_;
  int offsetMinutes_;
  const std::chrono::time_zone* zone_;

  constexpr TimeZone(Kind kind, int offsetMinutes,
                     const std::chrono::time_zone* zone)
    : kind_(kind), offsetMinutes_(offsetMinutes), zone_(zone) {}

public:
  static constexpr TimeZone utc() { return TimeZone(Kind::UTC, 0, nullptr); }
  static constexpr TimeZone fixed(int offsetMinutes) {
    return offsetMinutes == 0 ? utc() : TimeZone(Kind::FIXED, offsetMinutes, nullptr);
  }
  static constexpr TimeZone iana(const std::chrono::time_zone* zone) {
    return TimeZone(Kind::IANA, 0, zone);
  }

  constexpr Kind kind() const { return kind_; }
  constexpr bool isUtc() const { return kind_ == Kind::UTC; }
  constexpr bool isFixed() const { return kind_ == Kind::FIXED; }
  constexpr bool isIana() const { return kind_ == Kind::IANA; }
  constexpr int offsetMinutes() const { return offsetMinutes_; }
  constexpr const std::chrono::time_zone* ianaZone() const { return zone_; }

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

struct TzdbState {
  std::once_flag once;
  const std::chrono::tzdb* database = nullptr;
  std::string version = "unavailable";
  std::string error;
};

inline TzdbState& tzdbState() {
  static TzdbState state;
  return state;
}

inline void loadTzdbOnce() {
  TzdbState& state = tzdbState();
  std::call_once(state.once, [&] {
    try {
      state.database = &std::chrono::get_tzdb();
      state.version = state.database->version;
      LOG_INFO("Loaded time-zone database version {}", state.version);
    } catch (const std::exception& e) {
      state.error = e.what();
      LOG_ERROR("Time-zone database unavailable (probed system zoneinfo at "
                "/usr/share/zoneinfo): {}. UTC and fixed offsets remain available",
                state.error);
    } catch (...) {
      state.error = "unknown error";
      LOG_ERROR("Time-zone database unavailable (probed system zoneinfo at "
                "/usr/share/zoneinfo): unknown error. UTC and fixed offsets remain available");
    }
  });
}

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

// Force the process tzdb snapshot to load at server startup. Failure is
// recorded rather than thrown so UTC/fixed-only service remains available.
inline void preWarmTimeZoneDatabase() {
  datetime_detail::loadTzdbOnce();
}

inline bool timeZoneDatabaseAvailable() {
  datetime_detail::loadTzdbOnce();
  return datetime_detail::tzdbState().database != nullptr;
}

inline std::string_view timeZoneDatabaseVersion() {
  datetime_detail::loadTzdbOnce();
  return datetime_detail::tzdbState().version;
}

inline std::string_view timeZoneDatabaseError() {
  datetime_detail::loadTzdbOnce();
  return datetime_detail::tzdbState().error;
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

  datetime_detail::loadTzdbOnce();
  auto* database = datetime_detail::tzdbState().database;
  if (database == nullptr) return std::nullopt;
  try {
    return TimeZone::iana(database->locate_zone(std::string(spec)));
  } catch (const std::exception&) {
    return std::nullopt;
  }
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

// The [start, end) window a date text denotes at its own granularity.
struct DateRange {
  int64_t lo;
  int64_t hiExclusive;
  bool granuleSkipped = false;
};

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

struct CivilFence {
  int64_t civilMillis;
  int64_t instant;
};

inline std::optional<CivilTime> civilFromInstant(int64_t millis, const TimeZone& zone) {
  namespace dd = datetime_detail;
  using namespace std::chrono;
  if (millis < dd::kMinEpochMs || millis > dd::kMaxEpochMs) return std::nullopt;

  seconds offset{0};
  if (zone.isFixed()) {
    offset = minutes{zone.offsetMinutes()};
  } else if (zone.isIana()) {
    try {
      auto info = zone.ianaZone()->get_info(sys_time<milliseconds>{milliseconds{millis}});
      offset = info.offset;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

  __int128 local = (__int128)millis + duration_cast<milliseconds>(offset).count();
  // This numerical guard precedes every year_month_day construction by callers
  // and prevents chrono::year's 16-bit representation from narrowing.
  if (local < dd::kMinEpochMs || local > dd::kMaxEpochMs) return std::nullopt;
  return CivilTime{(int64_t)local, offset};
}

namespace datetime_detail {

inline std::chrono::seconds offsetForLocalInfo(
    int64_t civilMillis, const std::chrono::local_info& info,
    std::optional<std::chrono::seconds> preferredOffset) {
  using namespace std::chrono;
  if (info.result == local_info::unique) return info.first.offset;
  if (info.result == local_info::ambiguous) {
    // libstdc++ 16 classifies the exact upper endpoint of some transition
    // intervals with the interval itself. Civil transition intervals are
    // half-open; at that endpoint only the post-transition offset is valid.
    auto transition = duration_cast<milliseconds>(
        info.first.end.time_since_epoch()).count();
    __int128 overlapEnd = (__int128)transition
                        + duration_cast<milliseconds>(info.first.offset).count();
    if ((__int128)civilMillis >= overlapEnd) return info.second.offset;
    // Java ZonedDateTime.ofLocal: retain a valid preferred source offset;
    // otherwise use the earlier (pre-transition) offset.
    return preferredOffset && *preferredOffset == info.second.offset
        ? info.second.offset : info.first.offset;
  }
  auto transition = duration_cast<milliseconds>(
      info.second.begin.time_since_epoch()).count();
  __int128 gapEnd = (__int128)transition
                  + duration_cast<milliseconds>(info.second.offset).count();
  if ((__int128)civilMillis >= gapEnd) return info.second.offset;
  // Java's gap shift is local - pre-transition offset. This deliberately
  // does not use to_sys(choose::), which clamps to the transition instant.
  return info.first.offset;
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
    try {
      auto info = zone.ianaZone()->get_info(
          local_time<milliseconds>{milliseconds{civilMillis}});
      offset = dd::offsetForLocalInfo(civilMillis, info, preferredOffset);
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  return dd::instantWithOffset(civilMillis, offset);
}

// Detect a nominal civil interval covered by a single forward offset jump.
// This also normalizes libstdc++ text-tzdb endpoint discrepancies such as
// Pacific/Apia's 2011 skipped date, where adjacent resolved starts alone do
// not identify the intended civil label reliably.
inline bool civilIntervalSkipped(
    int64_t civilLo, int64_t civilNext, const TimeZone& zone) {
  using namespace std::chrono;
  if (!zone.isIana() || civilNext <= civilLo) return false;
  try {
    auto info = zone.ianaZone()->get_info(
        local_time<milliseconds>{milliseconds{civilNext - 1}});
    int64_t jump = duration_cast<milliseconds>(
        info.second.offset - info.first.offset).count();
    return info.result == local_info::nonexistent
        && jump >= civilNext - civilLo;
  } catch (const std::exception&) {
    return false;
  }
}

namespace datetime_detail {

// Forward-only local resolver for dense civil fence sequences. A cached
// local_info interval avoids repeated tzdb searches while a sequence stays in
// one transition window. A jump outside the interval performs one direct
// get_info lookup instead of walking intervening transitions.
class LocalTimeResolver {
  TimeZone zone;
  std::optional<std::chrono::seconds> preferredOffset;
  std::chrono::local_info cachedInfo;
  __int128 cachedBegin = 0;
  __int128 cachedEnd = 0;
  bool cached = false;

  void cache(int64_t civilMillis, const std::chrono::local_info& info) {
    using namespace std::chrono;
    cachedInfo = info;
    if (info.result == local_info::ambiguous) {
      auto transition = duration_cast<milliseconds>(
          info.first.end.time_since_epoch()).count();
      cachedBegin = (__int128)transition
                  + duration_cast<milliseconds>(info.second.offset).count();
      cachedEnd = (__int128)transition
                + duration_cast<milliseconds>(info.first.offset).count();
    } else if (info.result == local_info::nonexistent) {
      auto transition = duration_cast<milliseconds>(
          info.second.begin.time_since_epoch()).count();
      cachedBegin = (__int128)transition
                  + duration_cast<milliseconds>(info.first.offset).count();
      cachedEnd = (__int128)transition
                + duration_cast<milliseconds>(info.second.offset).count();
    } else {
      cachedBegin = civilMillis;
      if (info.first.end == sys_seconds::max()) {
        cachedEnd = (__int128)kMaxEpochMs + 1;
      } else {
        auto next = zone.ianaZone()->get_info(info.first.end);
        auto transition = duration_cast<milliseconds>(
            info.first.end.time_since_epoch()).count();
        auto endOffset = std::min(info.first.offset, next.offset);
        cachedEnd = (__int128)transition
                  + duration_cast<milliseconds>(endOffset).count();
      }
    }
    cached = cachedBegin <= civilMillis && (__int128)civilMillis < cachedEnd;
  }

public:
  LocalTimeResolver(const TimeZone& zone,
                    std::optional<std::chrono::seconds> preferredOffset)
    : zone(zone), preferredOffset(preferredOffset) {}

  std::optional<int64_t> resolve(int64_t civilMillis) {
    using namespace std::chrono;
    if (!zone.isIana()) {
      return instantFromCivil(civilMillis, zone, preferredOffset);
    }
    try {
      if (!cached || (__int128)civilMillis < cachedBegin
          || (__int128)civilMillis >= cachedEnd) {
        auto info = zone.ianaZone()->get_info(
            local_time<milliseconds>{milliseconds{civilMillis}});
        cache(civilMillis, info);
      }
      seconds offset = offsetForLocalInfo(
          civilMillis, cachedInfo, preferredOffset);
      return instantWithOffset(civilMillis, offset);
    } catch (const std::exception&) {
      cached = false;
      return std::nullopt;
    }
  }
};

} // namespace datetime_detail

// Parse only the literal forms above. Date-math splitting calls this for each
// possible fixed anchor, so it deliberately requires the whole input.
inline std::optional<DateLiteralRange> parseDateLiteralRange(
    std::string_view s, const TimeZone& frameZone) {
  using namespace std::chrono;
  namespace dd = datetime_detail;
  Cursor input{s};
  if (input.atEnd() || input.size() > dd::kMaxLiteralParseLen) return std::nullopt;

  // Bare integer => epoch millis (OpenSearch epoch_millis).  A real ISO date
  // always carries a '-' between year and month, so an all-digit run (after an
  // optional leading sign) is unambiguous.  Detected with range-checked
  // algorithms; converted with from_chars, which reports overflow as an error
  // rather than wrapping (signed-overflow UB).
  {
    Cursor integer = input;
    bool leadingPlus = integer.consume('+');
    if (!leadingPlus) integer.consume('-');
    std::string_view digits = integer.takeWhile(dd::isAsciiDigit);
    if (!digits.empty() && integer.atEnd()) {
      int64_t v = 0;
      std::string_view token = input.slice(leadingPlus ? 1 : 0, input.size());
      auto [ptr, ec] = std::from_chars(token.begin(), token.end(), v);
      if (ec != std::errc{} || ptr != token.end()) return std::nullopt;
      if (v < dd::kMinEpochMs || v > dd::kMaxEpochMs) return std::nullopt;
      return DateLiteralRange{DateRange{v, v + 1}, false};
    }
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
          int scale = 100;  // first three fractional digits -> millis; rest truncated
          while (dd::isAsciiDigit(c.peek())) {
            if (scale > 0) { frac += (c.peek() - '0') * scale; scale /= 10; }
            c.advance();
          }
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

  bool granuleSkipped = false;
  if (!hasExplicitOffset && civilNext) {
    auto next = instantFromCivil(*civilNext, frameZone);
    granuleSkipped = next && *next == *lo;
    if (!granuleSkipped && frameZone.isIana()) {
      // libstdc++ 16's text-tzdb reader places Apia's 2011 dateline jump one
      // hour later than the system TZif file. The ofLocal result for the
      // skipped label remains correct (it depends only on the pre-offset), but
      // resolving the next label does not compare equal. If get_info reports a
      // forward jump at least as wide as the whole nominal granule containing
      // its last millisecond, this is the same zero-width-granule condition.
      granuleSkipped = civilIntervalSkipped(civilLo, *civilNext, frameZone);
    }
  }
  return DateLiteralRange{DateRange{*lo, *last + 1, granuleSkipped}, hasExplicitOffset};
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

// Evaluate once for the lower edge and once for the upper edge. A slash floors
// the lower edge and rounds the upper edge to the unit's last millisecond;
// later commands transform both. This is exactly the interval the existing
// DATE range fold needs for gte/gt/lte/lt semantics.
inline std::optional<DateRange> applyDateMath(int64_t anchor, std::string_view math,
                                              const TimeZone& zone) {
  Cursor cur{math};
  if (cur.atEnd()) return std::nullopt;
  int64_t lo = anchor;
  int64_t hi = anchor;
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
    } else {
      auto newLo = addUnit(lo, amount, *unit, zone);
      auto newHi = addUnit(hi, amount, *unit, zone);
      if (!newLo || !newHi || *newLo > *newHi) return std::nullopt;
      lo = *newLo;
      hi = *newHi;
    }
  }
  if (!supportedInstant(lo) || !supportedInstant(hi) || lo > hi) return std::nullopt;
  return DateRange{lo, hi + 1};
}

inline bool startsWithNow(const Cursor& input) {
  return input.startsWithAsciiIgnoreCase("NOW");
}

} // namespace datetime_detail

// From-start civil stepping for calendar facet fences. The start instant's
// frame offset is retained as the ofLocal preference for every independently
// computed fence.
class CivilCalendarStepper {
  TimeZone zone;
  std::optional<CivilTime> startCivil;
  datetime_detail::LocalTimeResolver resolver;

public:
  CivilCalendarStepper(int64_t startInstant, const TimeZone& zone)
    : zone(zone), startCivil(civilFromInstant(startInstant, zone)),
      resolver(zone, startCivil
          ? std::optional<std::chrono::seconds>{startCivil->offset}
          : std::nullopt) {}

  bool valid() const { return startCivil.has_value(); }

  std::optional<CivilFence> at(int64_t amount, CalendarUnit unit) {
    namespace dd = datetime_detail;
    if (!startCivil) return std::nullopt;

    std::optional<int64_t> civil;
    if (unit == CalendarUnit::DAY || unit == CalendarUnit::WEEK) {
      int64_t daysPerUnit = unit == CalendarUnit::WEEK ? 7 : 1;
      __int128 target = (__int128)startCivil->millis
                      + (__int128)amount * daysPerUnit * dd::kMsPerDay;
      if (target < dd::kMinEpochMs || target > dd::kMaxEpochMs) {
        return std::nullopt;
      }
      civil = (int64_t)target;
    } else if (unit == CalendarUnit::YEAR) {
      civil = dd::calendarCivil(
          startCivil->millis, amount, dd::DateMathUnit::YEAR);
    } else {
      __int128 months = amount;
      if (unit == CalendarUnit::QUARTER) months *= 3;
      if (months < std::numeric_limits<int64_t>::min()
          || months > std::numeric_limits<int64_t>::max()) {
        return std::nullopt;
      }
      civil = dd::calendarCivil(
          startCivil->millis, (int64_t)months, dd::DateMathUnit::MONTH);
    }
    if (!civil) return std::nullopt;
    auto instant = resolver.resolve(*civil);
    if (!instant) return std::nullopt;
    return CivilFence{*civil, *instant};
  }
};

inline std::optional<DateRange> parseDateRange(std::string_view s, int64_t nowEpochMillis,
                                               const TimeZone& zone) {
  namespace dd = datetime_detail;
  Cursor input{s};
  if (input.atEnd() || input.size() > dd::kMaxDateMathParseLen
      || !dd::supportedInstant(nowEpochMillis)) {
    return std::nullopt;
  }

  // NOW/now is the one non-literal anchor. Case-insensitive acceptance lets
  // Solr and OpenSearch clients share the same endpoint.
  if (dd::startsWithNow(input)) {
    if (input.size() == 3) return DateRange{nowEpochMillis, nowEpochMillis + 1};
    input.advance(3);
    input.consume("||");  // harmless union convenience
    return dd::applyDateMath(
        nowEpochMillis, input.slice(input.position(), input.size()), zone);
  }

  // A literal without math retains Solux's existing granularity window.
  if (auto literal = parseDateLiteralRange(input.slice(0, input.size()), zone)) {
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
    auto anchor = parseDateLiteralRange(input.slice(0, separator), zone);
    if (!anchor) return std::nullopt;
    auto result = dd::applyDateMath(
        anchor->range.lo, input.slice(mathStart, input.size()), zone);
    if (result) result->granuleSkipped = anchor->range.granuleSkipped;
    return result;
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
    auto anchor = parseDateLiteralRange(input.slice(0, separator), zone);
    if (!anchor) continue;
    if (auto result = dd::applyDateMath(
            anchor->range.lo, input.slice(separator, input.size()), zone)) {
      result->granuleSkipped = anchor->range.granuleSkipped;
      return result;
    }
  }
  return std::nullopt;
}

inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s,
                                                      int64_t nowEpochMillis,
                                                      const TimeZone& zone) {
  auto r = parseDateRange(s, nowEpochMillis, zone);
  if (!r) return std::nullopt;
  return r->lo;  // ingest/sort use the window start (an instant)
}

// Ingest has no request time-zone surface and intentionally remains UTC.
inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s,
                                                      int64_t nowEpochMillis) {
  return parseDateToEpochMillis(s, nowEpochMillis, TimeZone::utc());
}

inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s) {
  return parseDateToEpochMillis(s, currentEpochMillis(), TimeZone::utc());
}

// Render epoch millis as canonical UTC ISO-8601: 'YYYY-MM-DDThh:mm:ssZ', with
// '.fff' appended only when the sub-second part is non-zero (Solr's output
// form).  Used by tests and the (future) JSON rendering layer; the gRPC layer
// returns the raw millis column.
inline std::string formatEpochMillisIso8601(int64_t millis) {
  using namespace std::chrono;
  sys_time<milliseconds> tp{milliseconds{millis}};
  sys_days dp = floor<days>(tp);
  year_month_day ymd{dp};
  hh_mm_ss hms{tp - dp};
  int y = (int)ymd.year();
  unsigned mo = (unsigned)ymd.month();
  unsigned d = (unsigned)ymd.day();
  int hh = (int)hms.hours().count();
  int mm = (int)hms.minutes().count();
  int ss = (int)hms.seconds().count();
  int ms = (int)hms.subseconds().count();
  char buf[40];
  int len = ms != 0
    ? std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", y, mo, d, hh, mm, ss, ms)
    : std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, mo, d, hh, mm, ss);
  return std::string(buf, (size_t)len);
}

} // namespace solux
