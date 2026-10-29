// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "DateOutput.h"

#include <cmath>
#include <limits>

namespace luxir {

int64_t NumericDateOutput::operator()(int64_t value) const {
  __int128 converted = convertDateUnits(value, from, to);
  if (converted <= std::numeric_limits<int64_t>::min()
      || converted >= std::numeric_limits<int64_t>::max()) {
    throw RequestError("date output for " + source + " is outside the ns date range");
  }
  return (int64_t)converted;
}

double NumericDateOutput::operator()(double value) const {
  double converted = from == DateUnit::MILLIS ? value * 1000000.0 : value / 1000000.0;
  if (to == DateUnit::NANOS
      && (!std::isfinite(converted)
          || (long double)converted < (long double)std::numeric_limits<int64_t>::min() + 1
          || (long double)converted > (long double)std::numeric_limits<int64_t>::max() - 1)) {
    throw RequestError("date output for " + source + " is outside the ns date range");
  }
  return converted;
}

std::string_view IsoDateOutput::operator()(int64_t value) const {
  try {
    return api::build::arenaStr(mr, formatDateIso8601(value, unit));
  } catch (const std::out_of_range&) {
    throw RequestError("date_iso output for " + source + " is outside the date range");
  }
}

std::string_view IsoDateOutput::operator()(double value) const {
  long double tick = std::floor((long double)value);
  long double lo = unit == DateUnit::MILLIS ? (long double)datetime_detail::kMinEpochMs
      : (long double)std::numeric_limits<int64_t>::min() + 1;
  long double hi = unit == DateUnit::MILLIS ? (long double)datetime_detail::kMaxEpochMs
      : (long double)std::numeric_limits<int64_t>::max() - 1;
  if (!std::isfinite(value) || tick < lo || tick > hi) {
    throw RequestError("date_iso output for " + source + " is non-finite or outside the date range");
  }
  return (*this)((int64_t)tick);
}

} // namespace luxir
