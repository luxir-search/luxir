// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "luxir/util/ApiError.h"

namespace luxir {

enum class ValueType : uint8_t {
  INT64,
  DOUBLE,
  INT64_ARRAY,
  DOUBLE_ARRAY,
  COLUMN_ONLY,
};

// Logical meaning carried independently of the physical numeric lane. DATE
// values use the INT64/DOUBLE lanes but retain enough type information for
// expression and aggregate validation.
enum class ValueNature : uint8_t {
  NUMBER,
  DATE,
};

inline std::string_view valueNatureName(ValueNature nature) {
  switch (nature) {
    case ValueNature::NUMBER: return "number";
    case ValueNature::DATE: return "DATE";
  }
  return "unknown";
}

struct ResolvedValue {
  ValueType type = ValueType::INT64;
  ValueNature nature = ValueNature::NUMBER;
};

// An expression that cannot be evaluated as written (type mismatch, bad
// argument): a request error even when detected against document values.
class ValueEvaluationError : public ApiError {
public:
  explicit ValueEvaluationError(const std::string& message)
      : ApiError(ErrorKind::INVALID_REQUEST, "invalid_expression", message) {}
};

inline bool valueArray(ValueType type) {
  return type == ValueType::INT64_ARRAY || type == ValueType::DOUBLE_ARRAY;
}

inline bool valueDouble(ValueType type) {
  return type == ValueType::DOUBLE || type == ValueType::DOUBLE_ARRAY;
}

inline ValueType valueScalarType(ValueType type) {
  return valueDouble(type) ? ValueType::DOUBLE : ValueType::INT64;
}

inline ValueType valueArrayType(ValueType type) {
  return valueDouble(type) ? ValueType::DOUBLE_ARRAY : ValueType::INT64_ARRAY;
}

inline std::string_view valueTypeName(ValueType type) {
  switch (type) {
    case ValueType::INT64: return "int64";
    case ValueType::DOUBLE: return "double";
    case ValueType::INT64_ARRAY: return "int64 array";
    case ValueType::DOUBLE_ARRAY: return "double array";
    case ValueType::COLUMN_ONLY: return "non-numeric column";
  }
  return "unknown";
}

struct ValueArrayRef {
  uint32_t node = 0;
  int32_t docid = -1;
  float score = 0.0f;
  int64_t size = 0;
};

// Missing is independent of either numeric lane. An array is valid even when
// it is empty; missing means the expression had no value for the document.
struct ValueResult {
  ValueType type = ValueType::INT64;
  bool valid = false;
  int64_t intValue = 0;
  double doubleValue = 0.0;
  ValueArrayRef array;

  static ValueResult missing(ValueType type) {
    ValueResult out;
    out.type = type;
    return out;
  }

  static ValueResult integer(int64_t value) {
    ValueResult out;
    out.type = ValueType::INT64;
    out.valid = true;
    out.intValue = value;
    return out;
  }

  static ValueResult floating(double value) {
    ValueResult out;
    out.type = ValueType::DOUBLE;
    out.valid = true;
    out.doubleValue = value;
    return out;
  }

  static ValueResult arrayValue(ValueType type, uint32_t node, int32_t docid,
                                float score, int64_t size) {
    ValueResult out;
    out.type = type;
    out.valid = true;
    out.array = {node, docid, score, size};
    return out;
  }
};

// Compact scalar-only transport for aggregate batches. The ValueType is fixed
// by the resolved input program, so carrying the array reference and type tag
// from ValueResult through every document only burns cache bandwidth.
struct ScalarValueResult {
  union {
    int64_t intValue;
    double doubleValue;
  };
  bool valid = false;

  ScalarValueResult() : intValue(0) {}

  static ScalarValueResult integer(int64_t value) {
    ScalarValueResult out;
    out.intValue = value;
    out.valid = true;
    return out;
  }

  static ScalarValueResult floating(double value) {
    ScalarValueResult out;
    out.doubleValue = value;
    out.valid = true;
    return out;
  }
};

static_assert(sizeof(ScalarValueResult) == 16);

// BOUNDED means both endpoints are proven. UNBOUNDED is lack of knowledge,
// not a statement that the expression produces infinities. INVALID means the
// interval proves that at least one present input produces NaN, infinity, a
// domain error, or integer overflow.
enum class BoundsCertainty : uint8_t { BOUNDED, UNBOUNDED, INVALID };

enum class BoundsInvalidity : uint8_t {
  NONE,
  DOMAIN,
  NAN_VALUE,
  POSITIVE_INFINITY,
  NEGATIVE_INFINITY,
  INTEGER_OVERFLOW,
};

struct ValueBounds {
  ValueType type = ValueType::INT64;
  BoundsCertainty certainty = BoundsCertainty::UNBOUNDED;
  BoundsInvalidity invalidity = BoundsInvalidity::NONE;
  bool mayBeMissing = false;
  bool alwaysMissing = false;
  bool minAttained = false;
  bool maxAttained = false;
  int64_t intMin = std::numeric_limits<int64_t>::min();
  int64_t intMax = std::numeric_limits<int64_t>::max();
  double doubleMin = -std::numeric_limits<double>::infinity();
  double doubleMax = std::numeric_limits<double>::infinity();

  bool operator==(const ValueBounds&) const = default;

  static ValueBounds unbounded(ValueType type, bool mayBeMissing = false,
                               bool alwaysMissing = false) {
    ValueBounds out;
    out.type = type;
    out.mayBeMissing = mayBeMissing;
    out.alwaysMissing = alwaysMissing;
    return out;
  }

  static ValueBounds integer(int64_t min, int64_t max, bool mayBeMissing = false) {
    ValueBounds out;
    out.type = ValueType::INT64;
    out.certainty = BoundsCertainty::BOUNDED;
    out.mayBeMissing = mayBeMissing;
    out.intMin = min;
    out.intMax = max;
    out.minAttained = true;
    out.maxAttained = true;
    return out;
  }

  static ValueBounds floating(double min, double max, bool mayBeMissing = false) {
    ValueBounds out;
    out.type = ValueType::DOUBLE;
    out.certainty = BoundsCertainty::BOUNDED;
    out.mayBeMissing = mayBeMissing;
    out.doubleMin = min;
    out.doubleMax = max;
    out.minAttained = true;
    out.maxAttained = true;
    return out;
  }

  static ValueBounds invalid(ValueType type, BoundsInvalidity invalidity,
                             bool mayBeMissing = false) {
    ValueBounds out = unbounded(type, mayBeMissing);
    out.certainty = BoundsCertainty::INVALID;
    out.invalidity = invalidity;
    return out;
  }
};

inline double boundMinAsDouble(const ValueBounds& bounds) {
  return valueDouble(bounds.type) ? bounds.doubleMin : (double)bounds.intMin;
}

inline double boundMaxAsDouble(const ValueBounds& bounds) {
  return valueDouble(bounds.type) ? bounds.doubleMax : (double)bounds.intMax;
}

} // namespace luxir
