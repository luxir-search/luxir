#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

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
// Returns nullopt if the text is not a recognized form.  Fractional seconds
// beyond millisecond precision are truncated.
//
// parseDateRange additionally reports the time WINDOW the text denotes at its
// own granularity: '2024-06-25' is the whole day, '2024-06' the whole month,
// '...T10:30' the whole minute.  Queries use the window (equality on a day
// matches the day; range endpoints include the granule they name); ingest and
// sorting use the window start (an instant).

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

// Hard cap on input length.  A valid date is at most ~30 chars (9-digit year +
// full time + zone offset); rejecting longer inputs up front bounds the work an
// attacker can induce.  Generous, so no legitimate value is refused.
inline constexpr size_t kMaxParseLen = 64;

// Bounds-checked forward cursor over a string_view.  ALL input access in the
// parser goes through this type, so out-of-bounds reads are impossible by
// construction rather than merely absent: peek() past the end returns '\0'
// (never a valid date character), and every consuming method range-checks
// before it advances.  The parser body therefore does no raw indexing, and the
// no-overrun property is verified by inspecting this one class, not each of the
// caller's branches.
class Cursor {
  const char* cur;
  const char* end;

public:
  explicit Cursor(std::string_view s) : cur(s.data()), end(s.data() + s.size()) {}

  bool eof() const { return cur >= end; }

  // Current char, or '\0' at end of input.  '\0' is not a valid date character,
  // so a comparison against an expected char naturally fails at end with no
  // separate length check at the call site.
  char peek() const { return cur < end ? *cur : '\0'; }

  void advance() { if (cur < end) ++cur; }

  // Consume the current char iff it equals c.
  bool accept(char c) {
    if (cur < end && *cur == c) { ++cur; return true; }
    return false;
  }

  // Consume exactly n digits (0 <= n <= 9) into out; returns false without
  // advancing if fewer than n digits are available.
  bool fixedDigits(int n, int& out) {
    if (end - cur < n) return false;
    int v = 0;
    for (int i = 0; i < n; ++i) {
      char ch = cur[i];
      if (ch < '0' || ch > '9') return false;
      v = v * 10 + (ch - '0');
    }
    cur += n;
    out = v;
    return true;
  }

  // Consume 1..maxDigits leading digits into out; returns the count consumed
  // (0 = no digit present).  maxDigits keeps the accumulator from overflowing.
  int varDigits(int maxDigits, long& out) {
    long v = 0;
    int count = 0;
    while (cur < end && count < maxDigits && *cur >= '0' && *cur <= '9') {
      v = v * 10 + (*cur - '0');
      ++cur;
      ++count;
    }
    out = v;
    return count;
  }
};

} // namespace datetime_detail

// The [start, end) window a date text denotes at its own granularity.
struct DateRange {
  int64_t lo;
  int64_t hiExclusive;
};

inline std::optional<DateRange> parseDateRange(std::string_view s) {
  using namespace std::chrono;
  namespace dd = datetime_detail;
  if (s.empty() || s.size() > dd::kMaxParseLen) return std::nullopt;

  // Bare integer => epoch millis (OpenSearch epoch_millis).  A real ISO date
  // always carries a '-' between year and month, so an all-digit run (after an
  // optional leading sign) is unambiguous.  Detected with range-checked
  // algorithms; converted with from_chars, which reports overflow as an error
  // rather than wrapping (signed-overflow UB).
  {
    std::string_view body = s;
    if (body.front() == '+' || body.front() == '-') body.remove_prefix(1);
    if (!body.empty() &&
        std::all_of(body.begin(), body.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      int64_t v = 0;
      const char* start = s.data() + (s.front() == '+' ? 1 : 0);  // from_chars handles '-'
      auto [ptr, ec] = std::from_chars(start, s.data() + s.size(), v);
      if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
      if (v < dd::kMinEpochMs || v > dd::kMaxEpochMs) return std::nullopt;
      return DateRange{v, v + 1};
    }
  }

  dd::Cursor c(s);
  int sign = 1;
  if (c.accept('-')) sign = -1;
  else c.accept('+');

  // Variable-width year, then '-MM' and an optional '-DD'.
  long year = 0;
  if (c.varDigits(9, year) == 0) return std::nullopt;
  year *= sign;
  int mo = 0, day = 1;
  if (!c.accept('-') || !c.fixedDigits(2, mo)) return std::nullopt;
  bool haveDay = false;
  if (c.accept('-')) {
    if (!c.fixedDigits(2, day)) return std::nullopt;
    haveDay = true;
  }

  int hh = 0, mm = 0, ss = 0, frac = 0, offsetMin = 0;
  // The granule width the text pins down, as fixed millis; 0 = a calendar
  // month (variable width, handled by date arithmetic below).
  int64_t granuleMs = haveDay ? dd::kMsPerDay : 0;

  // Optional time: 'T'/'t'/' ' then hh[:mm[:ss[.fff]]] (a time needs a day).
  if (!c.eof() && haveDay) {
    char t = c.peek();
    if (t != 'T' && t != 't' && t != ' ') return std::nullopt;  // junk after date
    c.advance();
    if (!c.fixedDigits(2, hh)) return std::nullopt;
    granuleMs = 3600000LL;
    if (c.accept(':')) {
      if (!c.fixedDigits(2, mm)) return std::nullopt;
      granuleMs = 60000LL;
      if (c.accept(':')) {
        if (!c.fixedDigits(2, ss)) return std::nullopt;
        granuleMs = 1000LL;
        char dot = c.peek();
        if (dot == '.' || dot == ',') {
          c.advance();
          if (!(c.peek() >= '0' && c.peek() <= '9')) return std::nullopt;  // need >= 1 digit
          granuleMs = 1;
          int scale = 100;  // first three fractional digits -> millis; rest truncated
          while (c.peek() >= '0' && c.peek() <= '9') {
            if (scale > 0) { frac += (c.peek() - '0') * scale; scale /= 10; }
            c.advance();
          }
        }
      }
    }
    // Optional zone: 'Z'/'z' or +/-hh[:mm].
    if (!c.eof()) {
      char z = c.peek();
      if (z == 'Z' || z == 'z') {
        c.advance();
      } else if (z == '+' || z == '-') {
        int zsign = (z == '-') ? -1 : 1;
        c.advance();
        int zh = 0, zm = 0;
        if (!c.fixedDigits(2, zh)) return std::nullopt;
        c.accept(':');  // optional separator ('hh:mm' or 'hhmm')
        if (c.peek() >= '0' && c.peek() <= '9') {
          if (!c.fixedDigits(2, zm)) return std::nullopt;
        }
        if (zh > 23 || zm > 59) return std::nullopt;
        offsetMin = zsign * (zh * 60 + zm);
      } else {
        return std::nullopt;
      }
    }
  }
  if (!c.eof()) return std::nullopt;  // any leftover (incl. embedded NUL + junk) is invalid

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

  if (granuleMs != 0) {
    return DateRange{millis, millis + granuleMs};
  }
  // Month granularity: the window ends at the first instant of the next month.
  year_month_day next = (mo == 12)
      ? year_month_day{std::chrono::year{(int)year + 1}, std::chrono::month{1}, std::chrono::day{1}}
      : year_month_day{ymd.year(), std::chrono::month{(unsigned)mo + 1}, std::chrono::day{1}};
  if ((int)next.year() > dd::kMaxYear) {
    return DateRange{millis, dd::kMaxEpochMs + 1};
  }
  int64_t hiExclusive = sys_days{next}.time_since_epoch().count() * dd::kMsPerDay
                      - (int64_t)offsetMin * 60000LL;
  return DateRange{millis, hiExclusive};
}

inline std::optional<int64_t> parseDateToEpochMillis(std::string_view s) {
  auto r = parseDateRange(s);
  if (!r) return std::nullopt;
  return r->lo;  // ingest/sort use the window start (an instant)
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
