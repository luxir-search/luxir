// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Calendar parts of DATE values: year(d), month(d), hour(d, offset_minutes)
// and friends, evaluated in UTC or at a fixed offset from UTC. Every function
// takes a DATE value of either unit (arrays map element-wise) and returns an
// int64. IANA zones are not available here; pass the offset in minutes.

#include <algorithm>
#include <chrono>

#include "luxir/util/DateTime.h"
#include "luxir/value/ValueFunctionSupport.h"

namespace luxir {
namespace {

using namespace valuefn;

constexpr ValueFunction::Operand DATE_VALUE{
    .unit = ValueFunction::DateUnit::RAW, .kind = ValueFunction::Kind::DATE};
constexpr ValueFunction::Operand OFFSET{ValueFunction::DateUnit::RAW, false, true};

enum class DatePart : uint8_t {
  YEAR, MONTH, DAY_OF_MONTH, DAY_OF_WEEK, DAY_OF_YEAR, HOUR, MINUTE, SECOND,
  MILLIS, MILLIS_OF_DAY, WEEK_OF_YEAR, QUARTER, EPOCH_MILLIS,
};

struct PartRange {
  int64_t low;
  int64_t high;
};

constexpr PartRange rangeOf(DatePart part) {
  switch (part) {
    case DatePart::YEAR: return {datetime_detail::kMinYear, datetime_detail::kMaxYear};
    case DatePart::MONTH: return {1, 12};
    case DatePart::DAY_OF_MONTH: return {1, 31};
    case DatePart::DAY_OF_WEEK: return {1, 7};
    case DatePart::DAY_OF_YEAR: return {1, 366};
    case DatePart::HOUR: return {0, 23};
    case DatePart::MINUTE: return {0, 59};
    case DatePart::SECOND: return {0, 59};
    case DatePart::MILLIS: return {0, 999};
    case DatePart::MILLIS_OF_DAY: return {0, datetime_detail::kMsPerDay - 1};
    case DatePart::WEEK_OF_YEAR: return {1, 53};
    case DatePart::QUARTER: return {1, 4};
    case DatePart::EPOCH_MILLIS: return {INT64_MIN, INT64_MAX};
  }
  std::unreachable();
}

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// ISO 8601: week 1 holds the year's first Thursday.
int weeksInYear(int year) {
  auto p = [](int64_t y) {
    return ((y + floorDiv(y, 4) - floorDiv(y, 100) + floorDiv(y, 400)) % 7 + 7) % 7;
  };
  return 52 + ((p(year) == 4 || p(year - 1) == 3) ? 1 : 0);
}

int isoWeek(int year, int dayOfYear, int isoWeekday) {
  int week = (dayOfYear - isoWeekday + 10) / 7;
  if (week < 1) return weeksInYear(year - 1);
  if (week > weeksInYear(year)) return 1;
  return week;
}

// node.intValue2 holds the unit of the DATE operand (1 for nanoseconds) and
// node.intValue the fixed offset in milliseconds; prepare() sets both.
void prepareDatePart(ValueNode& node, std::span<const ValueNode* const> args) {
  node.intValue2 = args[0]->nature == ValueNature::DATE_NS ? 1 : 0;
  node.intValue = 0;
  if (args.size() > 1) {
    double minutes = numberOf(*args[1]);
    if (minutes != std::floor(minutes)
        || std::abs(minutes) > datetime_detail::kMaxLiteralOffsetMinutes) {
      throw std::runtime_error(
          "the offset must be a whole number of minutes between -1439 and 1439");
    }
    node.intValue = (int64_t)minutes * 60000;
  }
}

template <DatePart Part>
int64_t datePart(const ValueNode& node, int64_t raw, int32_t docid) {
  int64_t millis = dateEpochMillis(raw, node.intValue2 == 1 ? DateUnit::NANOS : DateUnit::MILLIS);
  if constexpr (Part == DatePart::EPOCH_MILLIS) {
    unused(docid);
    return millis;
  } else {
    // The civil calendar is only defined for the supported year range.
    if (millis < datetime_detail::kMinEpochMs - node.intValue
        || millis > datetime_detail::kMaxEpochMs - node.intValue) {
      runtimeInvalid(node, docid, "is outside the supported calendar range");
    }
    millis += node.intValue;
    int64_t days = floorDiv(millis, datetime_detail::kMsPerDay);
    int64_t millisOfDay = millis - days * datetime_detail::kMsPerDay;
    if constexpr (Part == DatePart::MILLIS_OF_DAY) return millisOfDay;
    if constexpr (Part == DatePart::HOUR) return millisOfDay / 3600000;
    if constexpr (Part == DatePart::MINUTE) return millisOfDay / 60000 % 60;
    if constexpr (Part == DatePart::SECOND) return millisOfDay / 1000 % 60;
    if constexpr (Part == DatePart::MILLIS) return millisOfDay % 1000;
    using namespace std::chrono;
    sys_days day{std::chrono::days{days}};
    year_month_day date{day};
    int year = (int)date.year();
    if constexpr (Part == DatePart::YEAR) return year;
    if constexpr (Part == DatePart::MONTH) return (int64_t)(unsigned)date.month();
    if constexpr (Part == DatePart::DAY_OF_MONTH) return (int64_t)(unsigned)date.day();
    if constexpr (Part == DatePart::QUARTER) return ((int64_t)(unsigned)date.month() - 1) / 3 + 1;
    int isoWeekday = (int)weekday{day}.iso_encoding();
    if constexpr (Part == DatePart::DAY_OF_WEEK) return isoWeekday;
    int dayOfYear = (int)(day - sys_days{date.year() / January / 1}).count() + 1;
    if constexpr (Part == DatePart::DAY_OF_YEAR) return dayOfYear;
    return isoWeek(year, dayOfYear, isoWeekday);
  }
}

ResolvedValue datePartResolve(std::span<const ResolvedValue> args) {
  return {valueArray(args[0].type) ? ValueType::INT64_ARRAY : ValueType::INT64,
          ValueNature::NUMBER};
}

template <DatePart Part>
struct DatePartFn {
  static ValueResult apply(const ValueNode& node, const ValueResult& input, int32_t docid) {
    if (!input.valid) return ValueResult::missing(ValueType::INT64);
    return ValueResult::integer(datePart<Part>(node, input.intValue, docid));
  }

  static ValueResult point(BoundValueProgram& program, const ValueNode& node,
                           int32_t docid, float score) {
    ValueResult input = program.evalNode(node.children[0], docid, score);
    if (!input.valid) return ValueResult::missing(node.type);
    if (valueArray(input.type)) {
      return ValueResult::arrayValue(node.type, nodeIndex(program, node), docid, score,
                                     input.array.size);
    }
    return apply(node, input, docid);
  }

  static ValueResult element(BoundValueProgram& program, const ValueNode& node,
                             const ValueArrayRef& array, int64_t index) {
    ValueResult value = program.evalNode(node.children[0], array.docid, array.score);
    ValueResult item = valueArray(value.type) ? program.evalArrayElement(value.array, index) : value;
    return apply(node, item, array.docid);
  }

  static ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds> args) {
    if (args[0].alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
    constexpr PartRange range = rangeOf(Part);
    if constexpr (range.low == INT64_MIN) {
      return ValueBounds::unbounded(node.type, args[0].mayBeMissing);
    } else {
      ValueBounds out = ValueBounds::integer(range.low, range.high, args[0].mayBeMissing);
      out.type = node.type;
      out.minAttained = false;
      out.maxAttained = false;
      return out;
    }
  }
};

template <DatePart Part>
constexpr ValueFunction part(std::string_view name, uint8_t maxArity) {
  using F = DatePartFn<Part>;
  return {ValueOpcode::NONE, name, datePartResolve, F::point, evalFunctionBatch, F::bounds,
          F::element, 1, maxArity, {DATE_VALUE, OFFSET}, nullptr, prepareDatePart};
}

constexpr ValueFunction FUNCTIONS[] = {
    part<DatePart::YEAR>("year", 2),
    part<DatePart::MONTH>("month", 2),
    part<DatePart::DAY_OF_MONTH>("day_of_month", 2),
    part<DatePart::DAY_OF_WEEK>("day_of_week", 2),
    part<DatePart::DAY_OF_YEAR>("day_of_year", 2),
    part<DatePart::HOUR>("hour", 2),
    part<DatePart::MINUTE>("minute", 2),
    part<DatePart::SECOND>("second", 2),
    part<DatePart::MILLIS>("millis", 2),
    part<DatePart::MILLIS_OF_DAY>("millis_of_day", 2),
    part<DatePart::WEEK_OF_YEAR>("week_of_year", 2),
    part<DatePart::QUARTER>("quarter", 2),
    part<DatePart::EPOCH_MILLIS>("epoch_millis", 1),
};

static_assert(badOperandFlags(FUNCTIONS).empty(), badOperandFlags(FUNCTIONS));

} // namespace

std::span<const ValueFunction> valueDateFunctions() {
  return FUNCTIONS;
}

} // namespace luxir
