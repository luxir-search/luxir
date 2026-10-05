// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <cstdint>
#ifdef __APPLE__
#include <date/tz.h>
#endif

namespace luxir {
#ifdef __APPLE__
namespace time_zone_database = date;
#else
namespace time_zone_database = std::chrono;
#endif

// date and chrono use different local clock types, but identical durations.
inline time_zone_database::local_time<std::chrono::milliseconds>
timeZoneLocalMillis(int64_t millis) {
  return time_zone_database::local_time<std::chrono::milliseconds>{
      std::chrono::milliseconds{millis}};
}
}  // namespace luxir
