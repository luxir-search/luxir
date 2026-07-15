// Out-of-line definitions for the FieldType coercion virtuals (declared in
// FieldType.h, shared core in ValCoerce.h).  Kept out of the headers so
// FieldType.h stays independent of the api types.

#include "solux/schema/ValCoerce.h"

#include "solux/schema/FieldType.h"
#include "solux/util/DateTime.h"
#include "solux/util/NumericUtils.h"

namespace solux {

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
    if (auto ms = parseDateToEpochMillis(*s, now)) return *ms;
    throw std::runtime_error(fmt::format(
        "DATE field '{}': cannot parse '{}' as a date "
        "(expected ISO-8601, epoch millis, or date math)",
        fieldName, *s));
  }
  coerce::throwCoerce(fieldName, val, "a date");
}

std::pair<int64_t, int64_t> DateFieldType::coerceDateRange(const api::Val& val,
                                                           std::string_view fieldName,
                                                           const CoerceContext& context) const {
  if (auto i = std::get_if<int64_t>(&val.kind)) return {*i, *i + 1};  // an exact instant
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    int64_t now = context.dateMathNowEpochMillis.value_or(currentEpochMillis());
    if (auto r = parseDateRange(*s, now)) return {r->lo, r->hiExclusive};
    throw std::runtime_error(fmt::format(
        "DATE field '{}': cannot parse '{}' as a date "
        "(expected ISO-8601, epoch millis, or date math)",
        fieldName, *s));
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

} // namespace solux
