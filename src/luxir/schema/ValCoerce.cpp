// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Out-of-line definitions for the FieldType coercion virtuals (declared in
// FieldType.h, shared core in ValCoerce.h).  Kept out of the headers so
// FieldType.h stays independent of the api types.

#include "luxir/schema/ValCoerce.h"

#include "luxir/schema/FieldType.h"
#include <array>
#include "luxir/util/DateTime.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/IpAddress.h"

namespace luxir::coerce {

std::optional<int64_t> parseScaledDecimal(std::string_view s, int32_t scale,
    DecimalRounding rounding, DecimalError& error) {
  // Keep the integer digits, the round digit, and a sticky bit. Fraction
  // length and exponent only place the decimal point; neither needs a buffer.
  error = DecimalError::INVALID;
  if (scale < 0 || scale > 18) return std::nullopt;
  size_t i = 0;
  bool negative = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) negative = s[i++] == '-';
  std::array<uint8_t, 20> digits{};
  int64_t significant = 0, fraction = 0;
  bool sawDigit = false, point = false, sticky = false;
  for (; i < s.size(); i++) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      sawDigit = true;
      if (point) fraction++;
      if (significant == 0 && c == '0') continue;
      if (significant < (int64_t)digits.size()) digits[(size_t)significant] = c - '0';
      else sticky |= c != '0';
      significant++;
    } else if (c == '.' && !point) point = true;
    else break;
  }
  if (!sawDigit) return std::nullopt;
  int64_t exponent = 0;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    bool minus = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) minus = s[i++] == '-';
    size_t start = i;
    int64_t cap = (int64_t)std::min(s.size(), (size_t)(INT64_MAX / 4)) + 64;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
      exponent = exponent > cap / 10 ? cap : std::min(cap, exponent * 10 + s[i] - '0');
    }
    if (i == start) return std::nullopt;
    if (minus) exponent = -exponent;
  }
  if (i != s.size()) return std::nullopt;
  if (significant == 0) return 0;
  __int128 wholeDigits = (__int128)significant + exponent - fraction + scale;
  error = negative ? DecimalError::BELOW_RANGE : DecimalError::ABOVE_RANGE;
  if (wholeDigits > 19) return std::nullopt;
  uint64_t magnitude = 0;
  for (int k = 0; k < wholeDigits; k++) {
    magnitude = magnitude * 10 + (k < significant ? digits[k] : 0);
  }
  uint8_t round = 0;
  bool remainder = false;
  if (wholeDigits < 0) remainder = true;
  else if (wholeDigits < significant) {
    round = digits[(size_t)wholeDigits];
    remainder = sticky;
    for (size_t k = (size_t)wholeDigits + 1; k < digits.size(); k++) remainder |= digits[k] != 0;
  }
  bool fractional = round != 0 || remainder;
  if (rounding == DecimalRounding::EXACT && fractional) {
    error = DecimalError::INEXACT;
    return std::nullopt;
  }
  bool increment = rounding == DecimalRounding::NEAREST_AWAY ? round >= 5
      : rounding == DecimalRounding::FLOOR ? negative && fractional
      : rounding == DecimalRounding::CEIL && !negative && fractional;
  magnitude += increment;
  uint64_t limit = (uint64_t)INT64_MAX + negative;
  if (magnitude > limit) return std::nullopt;
  if (negative && magnitude == (uint64_t{1} << 63)) return INT64_MIN;
  return negative ? -(int64_t)magnitude : (int64_t)magnitude;
}

std::optional<int64_t> parseScaledDecimal(const api::Val& value, int32_t scale,
    DecimalRounding rounding, DecimalError& error) {
  char buffer[TEXT_BUF_SIZE];
  std::string_view text;
  error = DecimalError::INVALID;
  if (auto* s = std::get_if<std::string_view>(&value.kind)) text = *s;
  else if (auto* i = std::get_if<int64_t>(&value.kind)) {
    text = {buffer, std::to_chars(buffer, buffer + sizeof(buffer), *i).ptr};
  } else if (auto* d = std::get_if<double>(&value.kind)) {
    text = {buffer, std::to_chars(buffer, buffer + sizeof(buffer), *d).ptr};
  } else if (auto* f = std::get_if<float>(&value.kind)) {
    text = {buffer, std::to_chars(buffer, buffer + sizeof(buffer), *f).ptr};
  } else return std::nullopt;
  return parseScaledDecimal(text, scale, rounding, error);
}

} // namespace luxir::coerce

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

__int128 FieldType::coerceGridValue(const api::Val& val, std::string_view fieldName,
    std::string_view role, const CoerceContext& context) const {
  unused(role);
  return coerceColInt64(val, fieldName, context);
}

__int128 DateFieldType::coerceGridValue(const api::Val& val, std::string_view fieldName,
    std::string_view role, const CoerceContext& context) const {
  unused(fieldName);
  const auto* number = std::get_if<int64_t>(&val.kind);
  if (!number) throw RequestError(fmt::format("a DATE {} must be integer {}",
      role, dateUnitName(context.dateInputUnit.value_or(unit))));
  return convertDateUnits(*number, context.dateInputUnit.value_or(unit), unit);
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

coerce::DecimalRounding boundRounding(FieldType::LiteralUse use) {
  using Use = FieldType::LiteralUse;
  switch (use) {
    case Use::GTE: case Use::LT: return coerce::DecimalRounding::CEIL;
    case Use::GT: case Use::LTE: return coerce::DecimalRounding::FLOOR;
    default: return coerce::DecimalRounding::NEAREST_AWAY;
  }
}

std::optional<FieldType::IntWindow> scaledWindow(const api::Val& value, int scale,
    coerce::DecimalRounding rounding, FieldType::LiteralUse use) {
  coerce::DecimalError error;
  if (auto point = coerce::parseScaledDecimal(value, scale, rounding, error)) {
    return literalWindow({*point, *point}, use);
  }
  using Error = coerce::DecimalError;
  using Use = FieldType::LiteralUse;
  if (error != Error::BELOW_RANGE && error != Error::ABOVE_RANGE) return std::nullopt;
  bool above = error == Error::ABOVE_RANGE;
  bool unbounded = (above && (use == Use::LTE || use == Use::LT))
      || (!above && (use == Use::GTE || use == Use::GT));
  return unbounded ? FieldType::IntWindow{} : FieldType::IntWindow{1, 0};
}

} // namespace

FieldType::IntWindow IntFieldType::coerceIntWindow(const api::Val& val, LiteralUse use,
    std::string_view fieldName, const CoerceContext& context) const {
  if (use == LiteralUse::MATCH || use == LiteralUse::MEMBER) {
    return FieldType::coerceIntWindow(val, use, fieldName, context);
  }
  if (auto window = tryCoerceIntWindow(val, use, fieldName, context)) return *window;
  coerce::throwCoerce(fieldName, val, "an integer range bound");
}

std::optional<FieldType::IntWindow> IntFieldType::tryCoerceIntWindow(const api::Val& val,
    LiteralUse use, std::string_view fieldName, const CoerceContext& context) const {
  if (use == LiteralUse::MATCH || use == LiteralUse::MEMBER) {
    return FieldType::tryCoerceIntWindow(val, use, fieldName, context);
  }
  return scaledWindow(val, 0, boundRounding(use), use);
}

int64_t DecimalFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
    const CoerceContext& context) const {
  unused(context);
  coerce::DecimalError error;
  if (auto point = coerce::parseScaledDecimal(val, scale, coerce::DecimalRounding::NEAREST_AWAY, error)) return *point;
  coerce::throwCoerce(fieldName, val, fmt::format("a decimal with scale {} ({})", scale,
      error == coerce::DecimalError::INVALID ? "not a decimal number" : "is out of range"));
}

std::optional<FieldType::IntWindow> DecimalFieldType::tryCoerceIntWindow(const api::Val& val,
    LiteralUse use, std::string_view fieldName, const CoerceContext& context) const {
  unused(fieldName);
  auto rounding = boundRounding(use);
  if (context.decimalScaled && (use == LiteralUse::MATCH || use == LiteralUse::MEMBER)) {
    rounding = coerce::DecimalRounding::EXACT;
  }
  return scaledWindow(val, context.decimalScaled ? 0 : scale, rounding, use);
}

FieldType::IntWindow DecimalFieldType::coerceIntWindow(const api::Val& val, LiteralUse use,
    std::string_view fieldName, const CoerceContext& context) const {
  if (auto window = tryCoerceIntWindow(val, use, fieldName, context)) return *window;
  coerce::throwCoerce(fieldName, val, context.decimalScaled ? "a raw scaled integer" : "a decimal literal");
}

__int128 DecimalFieldType::coerceGridValue(const api::Val& val, std::string_view fieldName,
    std::string_view role, const CoerceContext& context) const {
  unused(role);
  coerce::DecimalError error;
  if (auto point = coerce::parseScaledDecimal(val, context.decimalScaled ? 0 : scale, coerce::DecimalRounding::EXACT, error)) return *point;
  coerce::throwCoerce(fieldName, val, "an exact multiple of the decimal field's unit");
}

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

FieldType::TermWindow FieldType::coerceTermWindow(const api::Val& val,
    std::string_view fieldName, std::span<char> buf) const {
  auto term = coerceTerm(val, fieldName, buf);
  return {term, term};
}

std::optional<FieldType::TermWindow> FieldType::tryCoerceTermWindow(const api::Val& val,
    std::string_view fieldName, std::span<char> buf) const {
  try {
    return coerceTermWindow(val, fieldName, buf);
  } catch (const RequestError& e) {
    if (e.code != "invalid_value") throw;
    return std::nullopt;
  }
}

std::string_view IpFieldType::coerceTerm(const api::Val& val, std::string_view fieldName,
    std::span<char> buf) const {
  if (auto* text = std::get_if<std::string_view>(&val.kind)) {
    if (auto address = ip::parse(*text)) {
      if (buf.size() < ip::BYTES) throw std::logic_error("IP coercion buffer is too small");
      std::memcpy(buf.data(), address->data(), ip::BYTES);
      return {buf.data(), ip::BYTES};
    }
  }
  coerce::throwCoerce(fieldName, val, "an IP address");
}

std::optional<FieldType::TermWindow> IpFieldType::tryCoerceTermWindow(const api::Val& val,
    std::string_view fieldName, std::span<char> buf) const {
  unused(fieldName);
  if (auto* text = std::get_if<std::string_view>(&val.kind)) {
    if (buf.size() < 2 * ip::BYTES) throw std::logic_error("IP window buffer is too small");
    if (auto range = ip::parseCidr(*text)) {
      std::memcpy(buf.data(), range->lo.data(), ip::BYTES);
      std::memcpy(buf.data() + ip::BYTES, range->hi.data(), ip::BYTES);
      return TermWindow{{buf.data(), ip::BYTES}, {buf.data() + ip::BYTES, ip::BYTES}};
    }
    if (auto address = ip::parse(*text)) {
      std::memcpy(buf.data(), address->data(), ip::BYTES);
      return TermWindow{{buf.data(), ip::BYTES}, {buf.data(), ip::BYTES}};
    }
  }
  return std::nullopt;
}

FieldType::TermWindow IpFieldType::coerceTermWindow(const api::Val& val,
    std::string_view fieldName, std::span<char> buf) const {
  if (auto range = tryCoerceTermWindow(val, fieldName, buf)) return *range;
  coerce::throwCoerce(fieldName, val, "an IP address or CIDR");
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
