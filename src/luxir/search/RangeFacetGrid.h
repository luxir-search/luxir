// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <utility>
#include <vector>

#include "luxir/util/DateTime.h"
#include "luxir/schema/FieldType.h"
#include "luxir/search/RangeFacetLayout.h"

namespace luxir {

// Immutable value identity, independent of a facet's domain and counting span.
// All keys use the column encoding. DATE keys are instants; fixed DATE grids
// include both occurrences of a repeated civil time. No mutable state is shared.
class RangeFacetGrid {
public:
  struct Cell {
    int64_t key;
    // Global index for floating/calendar grids; fixed integer grids use key.
    int64_t index = 0;
  };
  struct Fences {
    std::vector<int64_t> values;
    int64_t affineGap = 0;
    std::vector<int64_t> skippedCivil;
  };

private:
  enum class Kind { FIXED, FLOATING, CALENDAR };
  Kind kind;
  FieldType::Type type;
  TimeZone zone;
  int64_t gap = 0;
  int64_t origin = 0;
  double floatingGap = 0;
  double floatingOrigin = 0;
  CalendarUnit unit = CalendarUnit::DAY;
  int64_t n = 1;
  int64_t anchor = 0;
  std::optional<std::chrono::seconds> preferredOffset;

  RangeFacetGrid(Kind kind, FieldType::Type type, TimeZone zone);
  template <typename Visit>
  void visitCalendarFences(Cell first, std::vector<int64_t>* skippedCivil,
                           Visit&& visit) const;
  Cell floatingCell(int64_t index) const;
  int64_t calendarCivil(int64_t index) const;
  Cell calendarCell(int64_t index) const;
  bool calendarSkipped(int64_t index) const;
  Cell dateFloor(int64_t value) const;
  Cell dateNext(Cell cell, int64_t limit) const;
  bool gapFence(int64_t transition, int64_t before, int64_t after) const;

public:
  static RangeFacetGrid fixed(FieldType::Type type, int64_t gap, int64_t origin = 0,
                             TimeZone zone = TimeZone::utc());
  static RangeFacetGrid floating(FieldType::Type type, double gap, double origin = 0);
  // EXPLICIT retains its from-start calendar grid and offset preference.
  // DATA uses the civil epoch (ISO Monday for weeks), resolving folds earlier.
  static RangeFacetGrid calendar(CalendarUnit unit, int64_t n, TimeZone zone,
                                std::optional<int64_t> start = std::nullopt);

  Cell floor(int64_t encodedValue) const;
  Cell next(Cell cell, std::vector<int64_t>* skippedCivil = nullptr) const;
  int64_t encode(double value) const;
  const TimeZone& timeZone() const { return zone; }
  // Affine INT and UTC/fixed-offset DATE grids use the full encoded range.
  // Calendar and IANA resolution are limited by the civil representation.
  std::pair<int64_t, int64_t> valueBounds() const {
    if (kind == Kind::CALENDAR || (type == FieldType::Type::DATE && zone.isIana())) {
      return {datetime_detail::kMinEpochMs, datetime_detail::kMaxEpochMs};
    }
    return {INT64_MIN, INT64_MAX};
  }

  // Certify [start,end), including any transition fences. A certificate is
  // just a representation choice; floor/next define the same fence set.
  std::optional<int64_t> affineGap(int64_t start, int64_t end) const;

  // Compress the canonical layout already traversed by the fence builder or
  // window. Only fixed integer/date and calendar DAY/WEEK grids use runs.
  // Equal adjacent strides certify each run, including clipped/transition cells.
  std::pmr::vector<RangeFacetRun> runs(std::span<const int64_t> fences,
      std::pmr::memory_resource* resource = std::pmr::get_default_resource()) const;

  // O(number of zone transitions), for execution-time calendar warnings.
  // Returns only skipped labels between surviving fences in [lo,end].
  std::vector<int64_t> skippedCalendarCells(int64_t lo, int64_t end) const;

  // clip=true: exact EXPLICIT endpoints. Otherwise cover inclusive [lo,hi].
  // The cap applies to the final, deduplicated bucket count.
  Fences fences(int64_t lo, int64_t hi, bool clip, size_t cap = 100000) const;
};

} // namespace luxir
