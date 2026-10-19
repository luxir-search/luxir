// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <gtest/gtest.h>
#include <vector>

#include "luxir/util/DateTime.h"

namespace luxir::test {

inline int64_t epoch(std::string_view text) {
  auto value = parseDateToEpochMillis(text);
  EXPECT_TRUE(value.has_value()) << text;
  return value.value_or(0);
}

inline TimeZone zone(std::string_view name) {
  auto value = resolveTimeZone(name);
  EXPECT_TRUE(value.has_value()) << name;
  return value.value_or(TimeZone::utc());
}

// The previous EXPLICIT calendar algorithm, kept as a compatibility fingerprint.
inline std::vector<int64_t> oldCalendarFences(int64_t start, int64_t end,
                                      CalendarUnit unit, TimeZone zone, int64_t n = 1) {
  auto anchor = civilFromInstant(start, zone).value();
  datetime_detail::LocalTimeResolver resolver(zone, anchor.offset);
  auto at = [&](int64_t index) {
    int64_t civil = civilCalendarAt(anchor.millis, index * n, unit).value();
    return CivilFence{civil, resolver.resolve(civil).value()};
  };
  std::vector<int64_t> values;
  values.reserve(256);
  values.push_back(start);
  std::optional<int64_t> shifted;
  for (int64_t i = 1; ; i++) {
    auto fence = at(i);
    bool substituted = shifted.has_value();
    int64_t instant = shifted.value_or(fence.instant);
    shifted.reset();
    if (!substituted && civilIntervalSkipped(fence.civilMillis, at(i + 1).civilMillis, zone)) {
      shifted = fence.instant;
      continue;
    }
    if (instant >= end) { values.push_back(end); return values; }
    if (instant > values.back()) values.push_back(instant);
  }
}
} // namespace luxir::test
