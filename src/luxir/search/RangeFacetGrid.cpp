// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/RangeFacetGrid.h"

#include <cmath>

#include "luxir/util/NumericUtils.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/luxir_util.h"

namespace luxir {
namespace {
using namespace std::chrono;
namespace dd = datetime_detail;

__int128 floorDiv(__int128 value, __int128 divisor) {
  __int128 q = value / divisor;
  return q - (value % divisor < 0);
}

int64_t fit(__int128 value) {
  if (value < INT64_MIN || value > INT64_MAX) {
    throw RequestError("range fence arithmetic overflow");
  }
  return (int64_t)value;
}

int64_t civilFit(__int128 value) {
  if (value < dd::kMinEpochMs || value > dd::kMaxEpochMs) {
    throw RequestError("range fence is outside the supported civil range");
  }
  return (int64_t)value;
}

// sys_info's open-ended seconds endpoints cannot be cast directly to millis.
int64_t beginMillis(const sys_info& info) {
  return (int64_t)std::max((__int128)dd::kMinEpochMs,
      (__int128)info.begin.time_since_epoch().count() * 1000);
}

int64_t endMillis(const sys_info& info) {
  return (int64_t)std::min((__int128)dd::kMaxEpochMs + 1,
      (__int128)info.end.time_since_epoch().count() * 1000);
}

sys_info infoAt(const TimeZone& zone, int64_t instant) {
  // Compare in seconds without converting open-ended sys_info endpoints to
  // milliseconds (that conversion can overflow).
  auto value = std::chrono::floor<seconds>(sys_time<milliseconds>{milliseconds{instant}});
  auto info = zone.ianaZone()->get_info(value);
  // Some tzdb implementations return their final interval even for values
  // at/after its end (libstdc++ at 32767-12-31). Traversal must not treat that
  // non-advancing endpoint as another transition and spin indefinitely.
  if (value < info.begin || value >= info.end) {
    throw RequestError("range fence is outside the time-zone database's supported interval");
  }
  return info;
}

int64_t offsetMillis(const sys_info& info) {
  return duration_cast<milliseconds>(info.offset).count();
}

void checkBucketCount(uint64_t count, size_t cap, bool clip) {
  if (count > cap) {
    throw RequestError("range exceeds the " + std::to_string(cap)
        + " bucket limit" + (clip ? "" : "; use a larger gap"));
  }
}

} // namespace

// A traversal owns its resolver and civil lookahead. Keeping the hot loop in
// one scope lets its scalar state stay in registers, as in the column walkers.
template <typename Visit>
void RangeFacetGrid::visitCalendarFences(
    Cell first, std::vector<int64_t>* skippedCivil, Visit&& visit) const {
  auto unit = this->unit;
  int64_t n = this->n, anchor = this->anchor, gap = this->gap;
  TimeZone zone = this->zone;
  dd::LocalTimeResolver resolver(zone, preferredOffset);
  int64_t index = first.index, previous = first.key;
  int64_t civil = calendarCivil(index);
  auto after = [&]() -> std::optional<int64_t> {
    if (gap) {
      // Exact DAY/WEEK civil addition; months/years retain the original anchor.
      if (gap > dd::kMaxEpochMs - civil) return std::nullopt;
      return civil + gap;
    }
    return civilCalendarAt(anchor, fit(((__int128)index + 1) * n), unit);
  };
  auto followingCivil = after();
  std::optional<int64_t> shifted;
  for (;;) {
    if (!followingCivil) {
      throw RequestError("calendar fence is outside the supported civil range");
    }
    int64_t previousCivil = civil;
    civil = *followingCivil;
    // Calendar indices are bounded by the supported civil range.
    ++index;
    followingCivil = after();
    bool substituted = shifted.has_value();
    auto instant = substituted ? shifted : resolver.resolve(civil);
    shifted.reset();
    if (!instant) {
      throw RequestError("calendar fence is outside the supported civil range");
    }
    if (!substituted && followingCivil
        && resolver.intervalSkipped(civil, *followingCivil)) {
      if (skippedCivil) skippedCivil->push_back(civil);
      shifted = instant;
      continue;
    }
    if (*instant < previous) throw RequestError("calendar fences are not increasing");
    if (*instant == previous) {
      if (skippedCivil) skippedCivil->push_back(previousCivil);
      continue;
    }
    if (!visit(Cell{*instant, index})) return;
    previous = *instant;
  }
}

RangeFacetGrid::RangeFacetGrid(Kind kind, FieldType::Type type, TimeZone zone)
    : kind(kind), type(type), zone(zone) {}

RangeFacetGrid RangeFacetGrid::fixed(FieldType::Type type, int64_t gap, int64_t origin,
                                    TimeZone zone) {
  if (type != FieldType::Type::INT && type != FieldType::Type::DATE) {
    throw RequestError("integer grid requires INT or DATE");
  }
  if (gap <= 0) throw RequestError("range gap must be > 0");
  RangeFacetGrid result(Kind::FIXED, type, type == FieldType::Type::DATE ? zone : TimeZone::utc());
  result.gap = gap;
  result.origin = origin;
  return result;
}

RangeFacetGrid RangeFacetGrid::floating(FieldType::Type type, double gap, double origin) {
  if (type != FieldType::Type::FLOAT && type != FieldType::Type::DOUBLE) {
    throw RequestError("floating grid requires FLOAT or DOUBLE");
  }
  if (!std::isfinite(gap) || gap <= 0 || !std::isfinite(origin)) {
    throw RequestError("range gap must be finite and > 0; origin must be finite");
  }
  RangeFacetGrid result(Kind::FLOATING, type, TimeZone::utc());
  result.floatingGap = gap;
  result.floatingOrigin = origin == 0 ? 0 : origin;
  return result;
}

RangeFacetGrid RangeFacetGrid::calendar(CalendarUnit unit, int64_t n, TimeZone zone,
                                       std::optional<int64_t> start) {
  if (n <= 0 || unit < CalendarUnit::DAY || unit > CalendarUnit::YEAR) {
    throw RequestError("calendar_gap requires n > 0 and a DAY/WEEK/MONTH/QUARTER/YEAR unit");
  }
  RangeFacetGrid result(Kind::CALENDAR, FieldType::Type::DATE, zone);
  result.unit = unit;
  result.n = n;
  if (start) {
    auto civil = civilFromInstant(*start, zone);
    if (!civil) throw RequestError("calendar bounds are outside the supported civil range");
    result.anchor = civil->millis;
    result.preferredOffset = civil->offset;
  } else {
    // The phase is civil, not the offset in force at an arbitrary source value.
    result.anchor = unit == CalendarUnit::WEEK ? -3 * dd::kMsPerDay : 0;
  }
  if (unit == CalendarUnit::DAY || unit == CalendarUnit::WEEK) {
    result.gap = fit((__int128)n * (unit == CalendarUnit::WEEK ? 7 : 1) * dd::kMsPerDay);
  }
  return result;
}

int64_t RangeFacetGrid::encode(double value) const {
  if (type == FieldType::Type::FLOAT) {
    float rounded = (float)value;
    if (!std::isfinite(rounded)) throw RequestError("fence is not finite after FLOAT rounding");
    return (int64_t)floatToSortableInt32(rounded);
  }
  if (!std::isfinite(value)) throw RequestError("fence is not finite after DOUBLE rounding");
  return doubleToSortableInt64(value);
}

RangeFacetGrid::Cell RangeFacetGrid::floatingCell(int64_t index) const {
  // Keep the historical EXPLICIT expression. DATA uses this same expression
  // with a global index, never a rounded first fence.
  double value = floatingOrigin + (double)index * floatingGap;
  return {encode(value), index};
}

int64_t RangeFacetGrid::calendarCivil(int64_t index) const {
  auto civil = civilCalendarAt(anchor, fit((__int128)index * n), unit);
  if (!civil) throw RequestError("calendar fence is outside the supported civil range");
  return *civil;
}

bool RangeFacetGrid::calendarSkipped(int64_t index) const {
  if (!zone.isIana()) return false;
  int64_t civil = calendarCivil(index);
  auto next = civilCalendarAt(anchor, fit(((__int128)index + 1) * n), unit);
  return next && civilIntervalSkipped(civil, *next, zone);
}

RangeFacetGrid::Cell RangeFacetGrid::calendarCell(int64_t index) const {
  int64_t civil = calendarCivil(index);
  // Preserve the skipped-day normalization used by calendar facets. On some
  // text tzdb versions the next civil label resolves an hour late; the skipped
  // label's shifted instant is the surviving next label's correct fence.
  if (zone.isIana()) {
    auto previous = civilCalendarAt(anchor, fit(((__int128)index - 1) * n), unit);
    if (previous && civilIntervalSkipped(*previous, civil, zone)) civil = *previous;
  }
  auto instant = instantFromCivil(civil, zone, preferredOffset);
  if (!instant) throw RequestError("calendar fence is outside the supported civil range");
  return {*instant, index};
}

bool RangeFacetGrid::gapFence(int64_t transition, int64_t before, int64_t after) const {
  if (after <= before) return false;
  __int128 lo = (__int128)transition + before;
  __int128 hi = (__int128)transition + after;
  __int128 k = -floorDiv((__int128)origin - lo, gap); // ceil((lo-origin)/gap)
  return (__int128)origin + k * gap < hi;
}

RangeFacetGrid::Cell RangeFacetGrid::dateFloor(int64_t value) const {
  civilFit(value);
  int64_t probe = value;
  for (;;) {
    auto info = infoAt(zone, probe);
    int64_t begin = beginMillis(info);
    int64_t offset = offsetMillis(info);
    __int128 candidate = (__int128)origin
        + floorDiv((__int128)probe + offset - origin, gap) * gap - offset;
    if (candidate >= begin) return {civilFit(candidate)};
    if (begin == dd::kMinEpochMs) return {civilFit(candidate)};
    auto previous = infoAt(zone, begin - 1);
    if (gapFence(begin, offsetMillis(previous), offset)) return {begin};
    probe = begin - 1;
  }
}

RangeFacetGrid::Cell RangeFacetGrid::dateNext(Cell cell, int64_t limit) const {
  civilFit(cell.key);
  int64_t probe = cell.key;
  for (;;) {
    auto info = infoAt(zone, probe);
    int64_t end = endMillis(info);
    int64_t offset = offsetMillis(info);
    __int128 candidate = (__int128)origin
        + (floorDiv((__int128)probe + offset - origin, gap) + 1) * gap - offset;
    if (candidate < end) return {candidate >= limit ? limit : civilFit(candidate)};
    if (end >= limit) return {limit};
    if (end > dd::kMaxEpochMs) return {civilFit(candidate)};
    auto following = infoAt(zone, end);
    if (gapFence(end, offset, offsetMillis(following))) return {end};
    // Include a regular fence at the transition, including the later fold.
    probe = end;
    int64_t nextOffset = offsetMillis(following);
    if (((__int128)probe + nextOffset - origin) % gap == 0) return {probe};
  }
}

RangeFacetGrid::Cell RangeFacetGrid::floor(int64_t value) const {
  if (kind == Kind::FLOATING) {
    double decoded = type == FieldType::Type::FLOAT ? (double)sortableInt32ToFloat((int32_t)value)
                                        : sortableInt64ToDouble(value);
    if (!std::isfinite(decoded)) throw RequestError("cannot floor a non-finite value");
    long double index = std::floor(((long double)decoded - floatingOrigin) / floatingGap);
    // At larger indices unit increments cannot yield distinct double fences.
    if (index <= (long double)INT64_MIN || index >= (long double)INT64_MAX) {
      throw RequestError("floating grid index overflow");
    }
    Cell cell = floatingCell((int64_t)index);
    while (cell.key > value) {
      Cell previous = floatingCell(fit((__int128)cell.index - 1));
      if (previous.key >= cell.key) throw RequestError("fences are not strictly increasing after floating rounding");
      cell = previous;
    }
    for (;;) {
      double nominal = floatingOrigin + (double)fit((__int128)cell.index + 1) * floatingGap;
      // floor is still defined when the next fence exceeds the finite value
      // representation. A covering span will reject that fence; a clipped
      // EXPLICIT span can end before it.
      if (nominal > decoded && (!std::isfinite(nominal)
          || (type == FieldType::Type::FLOAT && !std::isfinite((float)nominal)))) return cell;
      Cell following = next(cell);
      if (following.key > value) return cell;
      cell = following;
    }
  }
  if (kind == Kind::CALENDAR) {
    auto civil = civilFromInstant(value, zone);
    if (!civil) throw RequestError("calendar bounds are outside the supported civil range");
    __int128 index;
    if (gap) {
      index = floorDiv((__int128)civil->millis - anchor, gap);
    } else {
      year_month_day a{local_days{std::chrono::floor<days>(milliseconds{anchor})}};
      year_month_day v{local_days{std::chrono::floor<days>(milliseconds{civil->millis})}};
      int64_t months = ((int)v.year() - (int)a.year()) * 12LL
                     + (int)(unsigned)v.month() - (int)(unsigned)a.month();
      int64_t monthsPerCell = fit((__int128)n * (unit == CalendarUnit::YEAR ? 12
                                             : unit == CalendarUnit::QUARTER ? 3 : 1));
      index = floorDiv(months, monthsPerCell);
    }
    Cell cell = calendarCell(fit(index));
    while (calendarSkipped(cell.index) || cell.key > value) cell = calendarCell(fit((__int128)cell.index - 1));
    visitCalendarFences(cell, nullptr, [&](Cell following) LUXIR_INLINE {
      if (following.key > value) return false;
      cell = following;
      return true;
    });
    return cell;
  }
  if (type == FieldType::Type::DATE && zone.isIana()) return dateFloor(value);
  int64_t offset = zone.isFixed() ? (int64_t)zone.offsetMinutes() * 60000 : 0;
  return {fit((__int128)origin
      + floorDiv((__int128)value + offset - origin, gap) * gap - offset)};
}

RangeFacetGrid::Cell RangeFacetGrid::next(Cell cell, std::vector<int64_t>* skippedCivil) const {
  if (kind == Kind::FLOATING) {
    Cell result = floatingCell(fit((__int128)cell.index + 1));
    if (result.key <= cell.key) {
      throw RequestError("fence " + std::to_string(cell.index + 1)
          + " is not strictly increasing after " + (type == FieldType::Type::FLOAT ? "FLOAT" : "DOUBLE") + " rounding");
    }
    return result;
  }
  if (kind == Kind::CALENDAR) {
    visitCalendarFences(cell, skippedCivil, [&](Cell following) LUXIR_INLINE {
      cell = following;
      return false;
    });
    return cell;
  }
  if (type == FieldType::Type::DATE && zone.isIana()) {
    return {civilFit(dateNext(cell, dd::kMaxEpochMs + 1).key)};
  }
  return {fit((__int128)cell.key + gap)};
}

std::optional<int64_t> RangeFacetGrid::affineGap(int64_t start, int64_t end) const {
  if (kind == Kind::FLOATING || (kind == Kind::CALENDAR && (!gap || zone.isIana()))) return std::nullopt;
  if (floor(start).key != start) return std::nullopt;
  if (!zone.isIana()) return gap;
  int64_t probe = start;
  for (;;) {
    auto info = infoAt(zone, probe);
    if (((__int128)start + offsetMillis(info) - origin) % gap != 0) return std::nullopt;
    int64_t transition = endMillis(info);
    if (transition >= end) return gap;
    auto following = infoAt(zone, transition);
    if (gapFence(transition, offsetMillis(info), offsetMillis(following))
        && ((__int128)transition - start) % gap != 0) return std::nullopt;
    probe = transition;
  }
}

std::pmr::vector<RangeFacetRun> RangeFacetGrid::runs(std::span<const int64_t> fences,
    std::pmr::memory_resource* resource) const {
  std::pmr::vector<RangeFacetRun> result(resource);
  if (fences.size() < 2 || kind == Kind::FLOATING || (kind == Kind::CALENDAR && !gap)) return result;
  size_t first = 0;
  while (first + 1 < fences.size()) {
    uint64_t stride = (uint64_t)fences[first + 1] - (uint64_t)fences[first];
    size_t end = first + 1;
    while (end + 1 < fences.size()
        && (uint64_t)fences[end + 1] - (uint64_t)fences[end] == stride) end++;
    result.push_back({fences[first], (uint64_t)fences[end] - (uint64_t)fences[first], stride, first});
    first = end;
  }
  return result;
}

std::vector<int64_t> RangeFacetGrid::skippedCalendarCells(int64_t lo, int64_t end) const {
  std::vector<int64_t> skipped;
  if (kind != Kind::CALENDAR || !zone.isIana()) return skipped;
  int64_t probe = lo;
  for (;;) {
    auto info = infoAt(zone, probe);
    int64_t transition = endMillis(info);
    if (transition > end || transition > dd::kMaxEpochMs) return skipped;
    auto following = infoAt(zone, transition);
    // The local resolver owns skipped-label normalization, including text
    // tzdb discrepancies between system and civil transition endpoints.
    if (offsetMillis(following) > offsetMillis(info)) {
      // The normalized surviving boundary can precede the sys_info jump
      // (Apia's text-tzdb endpoint differs by an hour). Step back from the
      // canonical boundary itself, not from the raw transition instant.
      auto boundary = floor(transition);
      if (boundary.key > lo) {
        auto previous = floor(boundary.key - 1);
        if (previous.key >= lo) next(previous, &skipped);
      }
    }
    probe = transition;
  }
}

RangeFacetGrid::Fences RangeFacetGrid::fences(int64_t lo, int64_t hi, bool clip, size_t cap) const {
  if (clip ? lo >= hi : lo > hi) throw RequestError("range start must be less than end");
  Cell first = kind == Kind::FLOATING && clip && floatingCell(0).key == lo
      ? floatingCell(0) : floor(lo);
  int64_t start = clip ? lo : first.key;
  int64_t end = hi;
  if (!clip) end = next(floor(hi)).key;
  Fences result;
  auto affine = affineGap(start, end);
  if (affine) {
    uint64_t distance = (uint64_t)end - (uint64_t)start;
    uint64_t count = distance / (uint64_t)*affine + (distance % (uint64_t)*affine != 0);
    checkBucketCount(count, cap, clip);
    result.values.resize((size_t)count + 1);
    for (size_t i = 0; i <= count; i++) {
      __uint128_t offset = (__uint128_t)i * (uint64_t)*affine;
      result.values[i] = (int64_t)((uint64_t)start + (uint64_t)std::min(offset, (__uint128_t)distance));
    }
    result.affineGap = *affine;
    return result;
  }
  result.values.reserve(256);
  result.values.push_back(start);
  auto append = [&](Cell cell) LUXIR_INLINE {
    result.values.push_back(std::min(cell.key, end));
    checkBucketCount(result.values.size() - 1, cap, clip);
    return cell.key < end;
  };
  if (kind == Kind::CALENDAR) {
    visitCalendarFences(first, &result.skippedCivil, append);
    return result;
  }
  Cell cell = first;
  for (;;) {
    // Clip without forming an overflowing next fence for bounded integer spans.
    if (kind == Kind::FIXED && !zone.isIana() && (__int128)cell.key + gap >= end) {
      append(Cell{end});
      return result;
    }
    if (kind == Kind::FLOATING && clip) {
      double nominal = floatingOrigin + (double)fit((__int128)cell.index + 1) * floatingGap;
      double last = type == FieldType::Type::FLOAT ? (double)sortableInt32ToFloat((int32_t)end)
                                       : sortableInt64ToDouble(end);
      if (nominal >= last) {
        append(Cell{end});
        return result;
      }
    }
    // A clipped DATE span need not have a representable canonical successor
    // beyond end (a very large gap may have none in the supported civil range).
    cell = kind == Kind::FIXED && type == FieldType::Type::DATE && zone.isIana()
        ? dateNext(cell, end) : next(cell, &result.skippedCivil);
    if (!append(cell)) return result;
  }
}

} // namespace luxir
