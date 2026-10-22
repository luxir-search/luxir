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
  coerce::throwCoerce(fieldName, val,
                      fmt::format("a column value (field type '{}' has no int column)", name_));
}

std::optional<int64_t> FieldType::tryCoerceColInt64(
    const api::Val& val, std::string_view fieldName, const CoerceContext& context) const {
  return coerceColInt64(val, fieldName, context);
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
  unused(context);
  if (auto b = std::get_if<bool>(&val.kind)) return *b ? 1 : 0;
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    if (*s == "true") return 1;
    if (*s == "false") return 0;
  }
  coerce::throwCoerce(fieldName, val, "a boolean (true or false)");
}

int64_t DecimalFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                         const CoerceContext& context) const {
  unused(context);
  if (auto i = std::get_if<int64_t>(&val.kind)) {
    int64_t scaled;
    int64_t factor = (int64_t)unit;
    if (!__builtin_mul_overflow(*i, factor, &scaled)) return scaled;
    coerce::throwCoerce(fieldName, val, fmt::format("a decimal with scale {} (out of range)", scale));
  }
  // Numbers parse through their shortest round-trip text, so 0.1 means the
  // decimal 0.1 rather than the binary double nearest it.
  char buf[coerce::TEXT_BUF_SIZE];
  std::string_view text;
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    text = *s;
  } else if (auto d = std::get_if<double>(&val.kind)) {
    text = std::string_view(buf, std::to_chars(buf, buf + sizeof(buf), *d).ptr);
  } else if (auto f = std::get_if<float>(&val.kind)) {
    text = std::string_view(buf, std::to_chars(buf, buf + sizeof(buf), *f).ptr);
  } else {
    coerce::throwCoerce(fieldName, val, "a decimal number");
  }
  const char* error = nullptr;
  if (auto scaled = coerce::parseScaledDecimal(text, scale, error)) return *scaled;
  throw RequestError(fmt::format("DECIMAL field '{}' (scale {}): '{}' {}",
                                 fieldName, scale, text, error), "invalid_value");
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

std::optional<int64_t> DateFieldType::tryCoerceColInt64(
    const api::Val& val, std::string_view fieldName, const CoerceContext& context) const {
  unused(fieldName);
  if (auto i = std::get_if<int64_t>(&val.kind)) return *i;
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    int64_t now = context.dateMathNowEpochMillis.value_or(currentEpochMillis());
    return parseDateToEpochMillis(*s, now, context.timeZone);
  }
  return std::nullopt;
}

namespace {

[[noreturn]] void throwDateCoerce(const api::Val& val, std::string_view fieldName) {
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    throw RequestError(fmt::format(
        "DATE field '{}': cannot parse '{}' as a date "
        "(expected ISO-8601, epoch millis, or date math)",
        fieldName, *s), "invalid_value");
  }
  coerce::throwCoerce(fieldName, val, "a date");
}

} // namespace

int64_t DateFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                      const CoerceContext& context) const {
  if (auto parsed = DateFieldType::tryCoerceColInt64(val, fieldName, context)) return *parsed;
  throwDateCoerce(val, fieldName);
}

std::optional<DateRange> DateFieldType::tryCoerceDateRange(
    const api::Val& val, const CoerceContext& context) const {
  if (auto i = std::get_if<int64_t>(&val.kind)) return DateRange{*i, *i + 1};
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    int64_t now = context.dateMathNowEpochMillis.value_or(currentEpochMillis());
    return parseDateRange(*s, now, context.timeZone);
  }
  return std::nullopt;
}

DateRange DateFieldType::coerceDateRange(const api::Val& val,
                                         std::string_view fieldName,
                                         const CoerceContext& context) const {
  if (auto parsed = tryCoerceDateRange(val, context)) return *parsed;
  throwDateCoerce(val, fieldName);
}

std::string_view TextFieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
                                           std::span<char> buf) const {
  return coerce::toText(val, fieldName, buf);
}

std::string_view StrFieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
                                          std::span<char> buf) const {
  return coerce::toText(val, fieldName, buf);
}

std::string_view IpFieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
                                         std::span<char> buf) const {
  unused(buf);
  // Address text passes through; ingest and query literals parse it to the
  // 16-byte term where STRING values are normalized.
  if (auto s = std::get_if<std::string_view>(&val.kind)) return *s;
  coerce::throwCoerce(fieldName, val, "an IP address");
}

std::string_view IdFieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
                                         std::span<char> buf) const {
  return coerce::toText(val, fieldName, buf);
}

} // namespace luxir
