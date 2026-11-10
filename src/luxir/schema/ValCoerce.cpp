// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Out-of-line definitions for the FieldType coercion virtuals (declared in
// FieldType.h, shared core in ValCoerce.h).  Kept out of the headers so
// FieldType.h stays independent of the api types.

#include "luxir/schema/ValCoerce.h"

#include "luxir/schema/FieldType.h"
#include "luxir/util/DateTime.h"
#include "luxir/util/NumericUtils.h"

namespace luxir {

int64_t FieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                  const CoerceContext& context) const {
  unused(context);
  unused(val, fieldName);
  throw RequestError(fmt::format("Field type '{}' has no int column", name_));
}

std::optional<int64_t> FieldType::tryCoerceColInt64(
    const api::Val& val, std::string_view fieldName, const CoerceContext& context) const {
  try {
    return coerceColInt64(val, fieldName, context);
  } catch (const RequestError& e) {
    if (e.code != "invalid_value") throw;
    return std::nullopt;
  }
}

namespace {

FieldType::IntWindow literalWindow(FieldType::IntWindow window, FieldType::LiteralUse use) {
  using Use = FieldType::LiteralUse;
  switch (use) {
    case Use::MATCH: case Use::MEMBER: return window;
    case Use::GTE: window.hi = INT64_MAX; break;
    case Use::GT:
      if (window.hi == INT64_MAX) return {1, 0, window.granuleSkipped, window.unroundedNow};
      window.lo = window.hi + 1;
      window.hi = INT64_MAX;
      break;
    case Use::LTE: window.lo = INT64_MIN; break;
    case Use::LT:
      if (window.lo == INT64_MIN) return {1, 0, window.granuleSkipped, window.unroundedNow};
      window.hi = window.lo - 1;
      window.lo = INT64_MIN;
      break;
  }
  return window;
}

} // namespace

FieldType::IntWindow FieldType::coerceIntWindow(
    const api::Val& val, LiteralUse use, std::string_view fieldName,
    const CoerceContext& context) const {
  int64_t value = coerceColInt64(val, fieldName, context);
  return literalWindow({value, value}, use);
}

std::optional<FieldType::IntWindow> FieldType::tryCoerceIntWindow(
    const api::Val& val, LiteralUse use, std::string_view fieldName,
    const CoerceContext& context) const {
  if (auto value = tryCoerceColInt64(val, fieldName, context)) {
    return literalWindow({*value, *value}, use);
  }
  return std::nullopt;
}

std::string_view FieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
                                       std::span<char> buf) const {
  unused(buf);
  coerce::throwCoerce(fieldName, val,
                      fmt::format("a term (field type '{}' is not term-backed)", name_));
}

int64_t IntFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                     const CoerceContext& context) const {
  unused(context);
  return coerce::toInt64(val, fieldName);
}

int64_t BoolFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                      const CoerceContext& context) const {
  if (auto value = tryCoerceColInt64(val, fieldName, context)) return *value;
  coerce::throwCoerce(fieldName, val, "a boolean (true or false)");
}

std::optional<int64_t> BoolFieldType::tryCoerceColInt64(
    const api::Val& val, std::string_view fieldName, const CoerceContext& context) const {
  unused(fieldName, context);
  if (auto b = std::get_if<bool>(&val.kind)) return *b ? 1 : 0;
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    if (*s == "true") return 1;
    if (*s == "false") return 0;
  }
  return std::nullopt;
}

int64_t FloatFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                       const CoerceContext& context) const {
  unused(context);
  return (int64_t)floatToSortableInt32(coerce::toFloat(val, fieldName));
}

int64_t DoubleFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                        const CoerceContext& context) const {
  unused(context);
  return doubleToSortableInt64(coerce::toDouble(val, fieldName));
}

std::optional<int64_t> IntFieldType::tryCoerceColInt64(
    const api::Val& val, std::string_view fieldName, const CoerceContext& context) const {
  unused(fieldName, context);
  return coerce::tryToInt64(val);
}

std::optional<int64_t> FloatFieldType::tryCoerceColInt64(
    const api::Val& val, std::string_view fieldName, const CoerceContext& context) const {
  unused(fieldName, context);
  if (auto parsed = coerce::tryToDouble(val)) return (int64_t)floatToSortableInt32((float)*parsed);
  return std::nullopt;
}

std::optional<int64_t> DoubleFieldType::tryCoerceColInt64(
    const api::Val& val, std::string_view fieldName, const CoerceContext& context) const {
  unused(fieldName, context);
  if (auto parsed = coerce::tryToDouble(val)) return doubleToSortableInt64(*parsed);
  return std::nullopt;
}

namespace {

struct DateColumnCoercion {
  std::optional<int64_t> value;
  bool outsideRange = false;
};

DateColumnCoercion tryDateColumn(const api::Val& val, DateUnit unit, const CoerceContext& context) {
  auto inputUnit = context.dateInputUnit.value_or(unit);
  std::optional<int64_t> integer;
  if (auto i = std::get_if<int64_t>(&val.kind)) integer = *i;
  if (auto s = std::get_if<std::string_view>(&val.kind)) integer = datetime_detail::epochInteger(*s);
  if (integer) {
    __int128 value = convertDateUnits(*integer, inputUnit, unit);
    if (unit == DateUnit::MILLIS
        && (value < datetime_detail::kMinEpochMs || value > datetime_detail::kMaxEpochMs)) {
      return {std::nullopt, true};
    }
    if (unit == DateUnit::NANOS && (value <= INT64_MIN || value >= INT64_MAX)) {
      return {std::nullopt, true};
    }
    return {(int64_t)value};
  }
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    int64_t now = context.dateMathNowEpochMillis.value_or(currentEpochMillis());
    if (auto parsed = parseDateRange(*s, now, context.timeZone, inputUnit)) {
      auto value = toDateUnits(parsed->lo, parsed->loNanos, unit);
      return {value, !value};
    }
  }
  return {};
}

[[noreturn]] void throwDateCoerce(const api::Val& val, std::string_view fieldName,
                                 DateUnit unit = DateUnit::MILLIS, bool outsideRange = false) {
  if (outsideRange) {
    auto range = unit == DateUnit::MILLIS ? "years -32767 to 32767"
        : "1677-09-21T00:12:43.145224193Z to 2262-04-11T23:47:16.854775806Z";
    throw RequestError(fmt::format("DATE field '{}': {} is outside the {} date range ({})",
        fieldName, coerce::describe(val), dateUnitName(unit), range), "invalid_value");
  }
  auto expected = fmt::format("a date (expected ISO-8601, epoch {}, or date math)", dateUnitName(unit));
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    throw RequestError(fmt::format("DATE field '{}': cannot parse '{}' as {}", fieldName, *s, expected),
                       "invalid_value");
  }
  coerce::throwCoerce(fieldName, val, expected);
}

} // namespace

int64_t DateFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                      const CoerceContext& context) const {
  auto parsed = tryDateColumn(val, unit, context);
  if (parsed.value) return *parsed.value;
  throwDateCoerce(val, fieldName, unit, parsed.outsideRange);
}

namespace {

std::optional<FieldType::IntWindow> tryDateWindow(
    const api::Val& val, DateUnit unit, const CoerceContext& context) {
  auto inputUnit = context.dateInputUnit.value_or(unit);
  std::optional<int64_t> integer;
  if (auto i = std::get_if<int64_t>(&val.kind)) integer = *i;
  if (auto s = std::get_if<std::string_view>(&val.kind)) integer = datetime_detail::epochInteger(*s);
  if (integer) {
    __int128 lo = convertDateUnits(*integer, inputUnit, unit);
    // A coarser numeric input names its entire input tick. Native numbers
    // are exact: no exclusive i+1 endpoint is constructed.
    int64_t width = dateUnitNanos(inputUnit) / dateUnitNanos(unit);
    return FieldType::IntWindow{clampDateUnits(lo), clampDateUnits(lo + std::max(width, (int64_t)1) - 1)};
  }
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    int64_t now = context.dateMathNowEpochMillis.value_or(currentEpochMillis());
    if (auto parsed = parseDateRange(*s, now, context.timeZone, inputUnit)) {
      int64_t tick = dateUnitNanos(unit);
      return FieldType::IntWindow{
          clampDateUnits(datetime_detail::toUnits128(parsed->lo, parsed->loNanos, tick, false)),
          clampDateUnits(datetime_detail::toUnits128(parsed->hiExclusive, parsed->hiNanos, tick, true) - 1),
          parsed->granuleSkipped, parsed->unroundedNow};
    }
  }
  return std::nullopt;
}

} // namespace

FieldType::IntWindow DateFieldType::coerceIntWindow(
    const api::Val& val, LiteralUse use, std::string_view fieldName,
    const CoerceContext& context) const {
  if (auto window = tryCoerceIntWindow(val, use, fieldName, context)) return *window;
  throwDateCoerce(val, fieldName, use == LiteralUse::MEMBER ? unit : context.dateInputUnit.value_or(unit));
}

std::optional<FieldType::IntWindow> DateFieldType::tryCoerceIntWindow(
    const api::Val& val, LiteralUse use, std::string_view fieldName,
    const CoerceContext& context) const {
  unused(fieldName);
  if (use == LiteralUse::MEMBER) {
    auto parsed = tryDateColumn(val, unit, context);
    if (parsed.value) return IntWindow{*parsed.value, *parsed.value};
    if (parsed.outsideRange) return IntWindow{1, 0};
    return std::nullopt;
  }
  if (auto window = tryDateWindow(val, unit, context)) return literalWindow(*window, use);
  return std::nullopt;
}

std::string_view TextFieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
                                           std::span<char> buf) const {
  return coerce::toText(val, fieldName, buf);
}

std::string_view StrFieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
                                          std::span<char> buf) const {
  return coerce::toText(val, fieldName, buf);
}

std::string_view IdFieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
                                         std::span<char> buf) const {
  return coerce::toText(val, fieldName, buf);
}

} // namespace luxir
