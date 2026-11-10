// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ValueOutput.h"

#include <cmath>
#include <limits>

namespace luxir {

namespace {

// Whether a double lies in the ns range [INT64_MIN + 1, INT64_MAX - 1].
// Doubles just below 2^63 in magnitude are 1024 apart, so that range holds
// exactly the doubles strictly between -2^63 and 2^63. NaN is outside.
bool insideNanos(double value) {
  return value > -0x1p63 && value < 0x1p63;
}

} // namespace

int64_t NumericValueOutput::operator()(int64_t value) const {
  __int128 converted = convertDateUnits(value, from, to);
  if (converted <= std::numeric_limits<int64_t>::min()
      || converted >= std::numeric_limits<int64_t>::max()) {
    throw RequestError("date output for " + context.source + " is outside the ns date range");
  }
  return (int64_t)converted;
}

double NumericValueOutput::operator()(double value) const {
  double converted = from == DateUnit::MILLIS ? value * 1000000.0 : value / 1000000.0;
  if (to == DateUnit::NANOS && !insideNanos(converted)) {
    throw RequestError("date output for " + context.source + " is outside the ns date range");
  }
  return converted;
}

std::string_view IsoValueOutput::operator()(int64_t value) const {
  try {
    return api::build::arenaStr(context.mr, formatDateIso8601(value, unit));
  } catch (const std::out_of_range&) {
    throw RequestError("date_iso output for " + context.source + " is outside the date range");
  }
}

std::string_view IsoValueOutput::operator()(double value) const {
  double tick = std::floor(value);
  bool inside = unit == DateUnit::MILLIS
      ? tick >= (double)datetime_detail::kMinEpochMs && tick <= (double)datetime_detail::kMaxEpochMs
      : insideNanos(tick);
  if (!inside) {
    throw RequestError("date_iso output for " + context.source + " is non-finite or outside the date range");
  }
  return (*this)((int64_t)tick);
}

} // namespace luxir
