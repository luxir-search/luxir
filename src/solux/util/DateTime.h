#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "solux/util/Clock.h"
#include "solux/util/Cursor.h"

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

// The [start, end) window a date text denotes at its own granularity.
struct DateRange {
  int64_t lo;
  int64_t hiExclusive;
};

// Parse only the literal forms above. Date-math splitting calls this for each
// possible fixed anchor, so it deliberately requires the whole input.
inline std::optional<DateRange> parseDateLiteralRange(std::string_view s) {
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
      return DateRange{v, v + 1};
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
        c.advance();
      } else if (z == '+' || z == '-') {
        int zsign = (z == '-') ? -1 : 1;
        c.advance();
        int zh = 0, zm = 0;
        if (!dd::fixedDigits(c, 2, zh)) return std::nullopt;
        c.consume(':');  // optional separator ('hh:mm' or 'hhmm')
        if (dd::isAsciiDigit(c.peek())) {
          if (!dd::fixedDigits(c, 2, zm)) return std::nullopt;
        }
        if (zh > 23 || zm > 59) return std::nullopt;
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
  int64_t millis = epochDays * 86400000LL
                 + (int64_t)hh * 3600000LL + (int64_t)mm * 60000LL
                 + (int64_t)ss * 1000LL + frac
                 - (int64_t)offsetMin * 60000LL;
  if (millis < dd::kMinEpochMs || millis > dd::kMaxEpochMs) return std::nullopt;

  if (granuleMs != 0) {
    return DateRange{millis, std::min(millis + granuleMs, dd::kMaxEpochMs + 1)};
  }
  // Month granularity: the window ends at the first instant of the next month.
  // Do not construct chrono::year{kMaxYear + 1}: year stores a 16-bit value
  // and would narrow before we could inspect it.
  if (mo == 12 && year == dd::kMaxYear) {
    int64_t hiExclusive = dd::kMaxEpochMs + 1 - (int64_t)offsetMin * 60000LL;
    return DateRange{millis, std::min(hiExclusive, dd::kMaxEpochMs + 1)};
  }
  year_month_day next = (mo == 12)
      ? year_month_day{std::chrono::year{(int)year + 1}, std::chrono::month{1}, std::chrono::day{1}}
      : year_month_day{ymd.year(), std::chrono::month{(unsigned)mo + 1}, std::chrono::day{1}};
  int64_t hiExclusive = sys_days{next}.time_since_epoch().count() * dd::kMsPerDay
                      - (int64_t)offsetMin * 60000LL;
  return DateRange{millis, std::min(hiExclusive, dd::kMaxEpochMs + 1)};
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

// Calendar addition in UTC with end-of-month clamping (Jan 31 + 1 month is
// the last day of February), matching Java time / Solr / OpenSearch behavior.
inline std::optional<int64_t> addCalendar(int64_t millis, int64_t amount,
                                          DateMathUnit unit) {
  using namespace std::chrono;
  sys_time<milliseconds> tp{milliseconds{millis}};
  sys_days dp = floor<days>(tp);
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
  int64_t out = sys_days{year_month_day{y, m, day{targetDay}}}.time_since_epoch().count()
              * kMsPerDay + timeOfDay.count();
  if (!supportedInstant(out)) return std::nullopt;
  return out;
}

inline std::optional<int64_t> addUnit(int64_t millis, int64_t amount,
                                      DateMathUnit unit) {
  switch (unit) {
    case DateMathUnit::YEAR:
    case DateMathUnit::MONTH:
      return addCalendar(millis, amount, unit);
    case DateMathUnit::WEEK: return addFixed(millis, amount, 7 * kMsPerDay);
    case DateMathUnit::DAY: return addFixed(millis, amount, kMsPerDay);
    case DateMathUnit::HOUR: return addFixed(millis, amount, 3600000LL);
    case DateMathUnit::MINUTE: return addFixed(millis, amount, 60000LL);
    case DateMathUnit::SECOND: return addFixed(millis, amount, 1000LL);
    case DateMathUnit::MILLISECOND: return addFixed(millis, amount, 1);
  }
  return std::nullopt;
}

// Return the inclusive [start,end] UTC interval containing millis at unit
// granularity. Week follows ISO/OpenSearch convention and starts Monday.
inline std::optional<std::pair<int64_t, int64_t>> roundedInterval(
    int64_t millis, DateMathUnit unit) {
  using namespace std::chrono;
  sys_time<milliseconds> tp{milliseconds{millis}};
  int64_t lo;
  int64_t hiExclusive;

  if (unit == DateMathUnit::MILLISECOND) return std::pair{millis, millis};
  if (unit == DateMathUnit::SECOND) {
    lo = floor<seconds>(tp).time_since_epoch().count() * 1000LL;
    hiExclusive = lo + 1000LL;
  } else if (unit == DateMathUnit::MINUTE) {
    lo = floor<minutes>(tp).time_since_epoch().count() * 60000LL;
    hiExclusive = lo + 60000LL;
  } else if (unit == DateMathUnit::HOUR) {
    lo = floor<hours>(tp).time_since_epoch().count() * 3600000LL;
    hiExclusive = lo + 3600000LL;
  } else {
    sys_days dp = floor<days>(tp);
    if (unit == DateMathUnit::DAY) {
      lo = dp.time_since_epoch().count() * kMsPerDay;
      hiExclusive = lo + kMsPerDay;
    } else if (unit == DateMathUnit::WEEK) {
      unsigned isoDay = weekday{dp}.iso_encoding();  // Monday=1 ... Sunday=7
      sys_days start = dp - days{isoDay - 1};
      lo = start.time_since_epoch().count() * kMsPerDay;
      hiExclusive = lo + 7 * kMsPerDay;
    } else {
      year_month_day ymd{dp};
      year y = ymd.year();
      month m = unit == DateMathUnit::YEAR ? month{1} : ymd.month();
      lo = sys_days{year_month_day{y, m, day{1}}}.time_since_epoch().count() * kMsPerDay;
      if (unit == DateMathUnit::YEAR) {
        hiExclusive = (int)y == kMaxYear
            ? kMaxEpochMs + 1
            : sys_days{year_month_day{y + years{1}, month{1}, day{1}}}
                  .time_since_epoch().count() * kMsPerDay;
      } else if ((int)y == kMaxYear && (unsigned)m == 12) {
        hiExclusive = kMaxEpochMs + 1;
      } else {
        year_month ym = y / m + months{1};
        hiExclusive = sys_days{year_month_day{ym.year(), ym.month(), day{1}}}
                          .time_since_epoch().count() * kMsPerDay;
      }
    }
  }

  if (lo < kMinEpochMs || lo > kMaxEpochMs) return std::nullopt;
  hiExclusive = std::min(hiExclusive, kMaxEpochMs + 1);
  if (hiExclusive <= lo) return std::nullopt;
  return std::pair{lo, hiExclusive - 1};
}

// Evaluate once for the lower edge and once for the upper edge. A slash floors
// the lower edge and rounds the upper edge to the unit's last millisecond;
// later commands transform both. This is exactly the interval the existing
// DATE range fold needs for gte/gt/lte/lt semantics.
inline std::optional<DateRange> applyDateMath(int64_t anchor, std::string_view math) {
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
      auto lowWindow = roundedInterval(lo, *unit);
      auto highWindow = roundedInterval(hi, *unit);
      if (!lowWindow || !highWindow) return std::nullopt;
      lo = lowWindow->first;
      hi = highWindow->second;
    } else {
      auto newLo = addUnit(lo, amount, *unit);
      auto newHi = addUnit(hi, amount, *unit);
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

inline std::optional<DateRange> parseDateRange(std::string_view s, int64_t nowEpochMillis) {
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
        nowEpochMillis, input.slice(input.position(), input.size()));
  }

  // A literal without math retains Solux's existing granularity window.
  if (auto literal = parseDateLiteralRange(input.slice(0, input.size()))) return literal;

  // OpenSearch uses an explicit anchor/math separator. It removes every
  // ambiguity with a partial time or numeric epoch anchor.
  Cursor separatorScan = input;
  if (separatorScan.findNext("||")) {
    size_t separator = separatorScan.position();
    separatorScan.advance(2);
    size_t mathStart = separatorScan.position();
    if (separatorScan.findNext("||")) return std::nullopt;
    auto anchor = parseDateLiteralRange(input.slice(0, separator));
    if (!anchor) return std::nullopt;
    return dd::applyDateMath(anchor->lo, input.slice(mathStart, input.size()));
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
    auto anchor = parseDateLiteralRange(input.slice(0, separator));
    if (!anchor) continue;
    if (auto result = dd::applyDateMath(
            anchor->lo, input.slice(separator, input.size()))) {
      return result;
    }
  }
  return std::nullopt;
}

inline std::optional<DateRange> parseDateRange(std::string_view s) {
  return parseDateRange(s, currentEpochMillis());
}

inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s,
                                                      int64_t nowEpochMillis) {
  auto r = parseDateRange(s, nowEpochMillis);
  if (!r) return std::nullopt;
  return r->lo;  // ingest/sort use the window start (an instant)
}

inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s) {
  return parseDateToEpochMillis(s, currentEpochMillis());
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
