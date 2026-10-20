// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Out-of-line definitions for the FieldType coercion virtuals (declared in
// FieldType.h, shared core in ValCoerce.h).  Kept out of the headers so
// FieldType.h stays independent of the api types.

#include "luxir/schema/ValCoerce.h"

#include "luxir/schema/FieldType.h"
#include "luxir/util/DateTime.h"
#include "luxir/util/NumericUtils.h"

#include <limits>
#include <memory>

namespace luxir {

int64_t FieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                  const CoerceContext& context) const {
  unused(context);
  coerce::throwCoerce(fieldName, val,
                      fmt::format("a column value (field type '{}' has no int column)", name_));
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

int64_t DateFieldType::coerceColInt64(const api::Val& val, std::string_view fieldName,
                                      const CoerceContext& context) const {
  // epoch millis passthrough; strings parse as ISO-8601 / epoch millis / date math
  if (auto i = std::get_if<int64_t>(&val.kind)) return *i;
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    int64_t now = context.dateMathNowEpochMillis.value_or(currentEpochMillis());
    if (auto ms = parseDateToEpochMillis(*s, now, context.timeZone)) return *ms;
    throw RequestError(fmt::format(
        "DATE field '{}': cannot parse '{}' as a date "
        "(expected ISO-8601, epoch millis, or date math)",
        fieldName, *s), "invalid_value");
  }
  coerce::throwCoerce(fieldName, val, "a date");
}

DateRange DateFieldType::coerceDateRange(const api::Val& val,
                                         std::string_view fieldName,
                                         const CoerceContext& context) const {
  if (auto i = std::get_if<int64_t>(&val.kind)) return {*i, *i + 1};  // an exact instant
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    int64_t now = context.dateMathNowEpochMillis.value_or(currentEpochMillis());
    if (auto r = parseDateRange(*s, now, context.timeZone)) return *r;
    throw RequestError(fmt::format(
        "DATE field '{}': cannot parse '{}' as a date "
        "(expected ISO-8601, epoch millis, or date math)",
        fieldName, *s), "invalid_value");
  }
  coerce::throwCoerce(fieldName, val, "a date");
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

static std::shared_ptr<FieldType> rangeElement(std::string_view name, FieldType::Type type) {
  switch (type) {
    case FieldType::INT: return std::make_shared<IntFieldType>(name);
    case FieldType::DOUBLE: return std::make_shared<DoubleFieldType>(name);
    case FieldType::DATE: return std::make_shared<DateFieldType>(name);
    default: throw std::invalid_argument("range fields hold INT, DOUBLE or DATE endpoints");
  }
}

RangeFieldType::RangeFieldType(std::string_view name, FieldType::Type elementType, int flags)
  : FieldType(name, FieldType::RANGE, flags), element(rangeElement(name, elementType)) {}

std::string_view RangeFieldType::typeName() const {
  switch (elementType()) {
    case FieldType::DOUBLE: return "double_range";
    case FieldType::DATE: return "date_range";
    default: return "long_range";
  }
}

std::optional<RangeFieldType::Bounds> RangeFieldType::encodeBounds(
    const api::Val* gte, const api::Val* gt, const api::Val* lte, const api::Val* lt,
    std::string_view fieldName, const CoerceContext& context) const {
  bool hasGte = gte && !coerce::isNull(*gte);
  bool hasGt = gt && !coerce::isNull(*gt);
  bool hasLte = lte && !coerce::isNull(*lte);
  bool hasLt = lt && !coerce::isNull(*lt);
  if (hasGte && hasGt) {
    throw RequestError(fmt::format("range on '{}' sets both gte and gt", fieldName), "invalid_value");
  }
  if (hasLte && hasLt) {
    throw RequestError(fmt::format("range on '{}' sets both lte and lt", fieldName), "invalid_value");
  }

  // Coerce every supplied bound before deciding emptiness, so a malformed
  // bound is always an error. Exclusive bounds start at their inclusive
  // neighbor's edge and step one encoded unit below.
  std::optional<int64_t> loEnc, hiEnc;
  bool unroundedNow = false;
  if (elementType() == FieldType::DATE) {
    auto& date = (const DateFieldType&)*element;
    auto dateBound = [&](const api::Val& value) {
      DateRange range = date.coerceDateRange(value, fieldName, context);
      unroundedNow |= range.unroundedNow;
      return range;
    };
    if (hasGte) loEnc = dateBound(*gte).lo;
    if (hasGt) loEnc = dateBound(*gt).hiExclusive - 1;
    if (hasLte) hiEnc = dateBound(*lte).hiExclusive - 1;
    if (hasLt) hiEnc = dateBound(*lt).lo;
  } else {
    if (hasGte || hasGt) loEnc = element->coerceColInt64(hasGte ? *gte : *gt, fieldName, context);
    if (hasLte || hasLt) hiEnc = element->coerceColInt64(hasLte ? *lte : *lt, fieldName, context);
  }

  Bounds bounds{std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(),
                unroundedNow};
  if (hasGte) {
    bounds.lo = *loEnc;
  } else if (hasGt) {
    if (*loEnc == std::numeric_limits<int64_t>::max()) return std::nullopt;
    bounds.lo = *loEnc + 1;
  }
  if (hasLte) {
    bounds.hi = *hiEnc;
  } else if (hasLt) {
    if (*hiEnc == std::numeric_limits<int64_t>::min()) return std::nullopt;
    bounds.hi = *hiEnc - 1;
  }
  if (bounds.lo > bounds.hi) return std::nullopt;
  return bounds;
}

} // namespace luxir
