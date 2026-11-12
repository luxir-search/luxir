// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Helpers shared by the value-function implementations: the core table in
// ValueFunctionRegistry.cpp and the function families that register next to it
// (math, dates, geo, hashing, local variables). Each family exports a constant
// table through the accessors declared in ValueFunctionRegistry.h.

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

#include <fmt/format.h>

#include "luxir/value/AggregateExpr.h"
#include "luxir/value/ValueExpr.h"
#include "luxir/value/ValueFunctionRegistry.h"

namespace luxir::valuefn {

inline void requireNumeric(std::span<const ResolvedValue> args) {
  for (const ResolvedValue& arg : args) {
    if (arg.nature == ValueNature::GEO_POINT) {
      throw std::runtime_error("GEO_POINT is not numeric; use geo_distance(), or geo_lat() and geo_lon()");
    }
    if (arg.type == ValueType::COLUMN_ONLY) {
      throw std::runtime_error(
          "a non-numeric column is only valid as a complete sort expression, "
          "as the argument of count(), or as the input of cardinality()");
    }
  }
}

inline void requireScalars(std::span<const ResolvedValue> args) {
  for (const ResolvedValue& arg : args) {
    if (valueArray(arg.type)) {
      // Not arrayReducerNames(): it resolves every function, including this one.
      throw std::runtime_error(
          "operands must be scalars; reduce an array first, for example with min() or max()");
    }
  }
}

inline void requireIntegers(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  for (const ResolvedValue& arg : args) {
    if (arg.type != ValueType::INT64) {
      throw std::runtime_error("operands must be integers; convert a double with to_int()");
    }
  }
}

[[noreturn]] inline void runtimeInvalid(const ValueNode& node, int32_t docid,
                                        std::string_view reason) {
  throw ValueEvaluationError(fmt::format(
      "value function {}() {} at segment doc {}", node.text, reason, docid));
}

inline bool invalid(const ValueBounds& bounds) {
  return bounds.certainty == BoundsCertainty::INVALID;
}

inline bool bounded(const ValueBounds& bounds) {
  return bounds.certainty == BoundsCertainty::BOUNDED;
}

inline ValueBounds retag(ValueBounds bounds, ValueType type) {
  bounds.type = type;
  return bounds;
}

// A bounds interval holding exactly one value, known to every document that
// has one.
inline bool singleton(const ValueBounds& bounds) {
  if (!bounded(bounds) || bounds.alwaysMissing) return false;
  if (valueDouble(bounds.type)) return bounds.doubleMin == bounds.doubleMax;
  return bounds.intMin == bounds.intMax;
}

inline double numberOf(const ValueResult& value) {
  return value.type == ValueType::DOUBLE ? value.doubleValue : (double)value.intValue;
}

inline double numberOf(const ValueNode& node) {
  return node.type == ValueType::DOUBLE ? node.doubleValue : (double)node.intValue;
}

inline ValueResult promote(ValueResult value, ValueType type) {
  if (!value.valid) return ValueResult::missing(type);
  if (valueArray(type)) {
    value.type = type;
    return value;
  }
  if (type == ValueType::DOUBLE && value.type == ValueType::INT64) {
    return ValueResult::floating((double)value.intValue);
  }
  value.type = type;
  return value;
}

inline ValueResult evalScalarOnlyElement(BoundValueProgram& program, const ValueNode& node,
                                         const ValueArrayRef& array, int64_t index) {
  unused(program);
  unused(node);
  unused(array);
  unused(index);
  throw std::logic_error("scalar value function used as an array expression");
}

inline void evalFunctionBatch(BoundValueProgram& program, const ValueNode& node,
                              std::span<const int32_t> docids, std::span<const float> scores,
                              std::span<ValueResult> results) {
  for (size_t i = 0; i < docids.size(); i++) {
    float score = scores.empty() ? 0.0f : scores[i];
    results[i] = node.function->evalPoint(program, node, docids[i], score);
  }
}

// The index of a node inside its program, for array results that re-evaluate
// the node lazily per element.
inline uint32_t nodeIndex(const BoundValueProgram& program, const ValueNode& node) {
  return (uint32_t)(&node - program.program.nodes.data());
}

inline ResolvedValue unaryDouble(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  return {valueArray(args[0].type) ? ValueType::DOUBLE_ARRAY : ValueType::DOUBLE,
          ValueNature::NUMBER};
}

// Bounds of a function whose result lies in a fixed interval whenever its
// operands are present. `mayBeMissing` / `alwaysMissing` carry over from the
// operands.
inline ValueBounds boundedResult(const ValueNode& node, double low, double high,
                                 bool mayBeMissing, bool alwaysMissing = false) {
  ValueBounds out = ValueBounds::floating(low, high, mayBeMissing);
  out.type = node.type;
  out.alwaysMissing = alwaysMissing;
  out.minAttained = false;
  out.maxAttained = false;
  return out;
}

inline ValueBounds unknownResult(const ValueNode& node, std::span<const ValueBounds> args) {
  bool mayBeMissing = false;
  bool alwaysMissing = false;
  for (const ValueBounds& arg : args) {
    mayBeMissing = mayBeMissing || arg.mayBeMissing;
    alwaysMissing = alwaysMissing || arg.alwaysMissing;
  }
  return ValueBounds::unbounded(node.type, mayBeMissing, alwaysMissing);
}

// The constant a double-valued function of constant operands produces, or the
// proof that it cannot produce a finite value.
inline ValueBounds constantDouble(const ValueNode& node, double value, bool mayBeMissing) {
  if (std::isnan(value)) {
    return ValueBounds::invalid(node.type, BoundsInvalidity::DOMAIN_ERROR, mayBeMissing);
  }
  if (std::isinf(value)) {
    return ValueBounds::invalid(node.type,
        value < 0 ? BoundsInvalidity::NEGATIVE_INFINITY : BoundsInvalidity::POSITIVE_INFINITY,
        mayBeMissing);
  }
  ValueBounds out = ValueBounds::floating(value, value, mayBeMissing);
  out.type = node.type;
  return out;
}

inline ValueBounds constantInteger(const ValueNode& node, int64_t value, bool mayBeMissing) {
  ValueBounds out = ValueBounds::integer(value, value, mayBeMissing);
  out.type = node.type;
  return out;
}

inline BucketScalar bucketDouble(double value) {
  if (!std::isfinite(value)) {
    return BucketScalar::failed(BucketValueType::DOUBLE, ValueNature::NUMBER,
                                AggregateFailure::NON_FINITE_EXPRESSION);
  }
  return BucketScalar::floating(value);
}

inline double bucketNumber(const BucketScalar& value) {
  return value.type == BucketValueType::DOUBLE ? value.doubleValue : (double)value.intValue;
}

// A family table is checked at compile time: every operand declares how DATE
// units combine, and a function with a bucket kernel takes no constant operand
// (bucket values are not constants).
consteval std::string_view badOperandFlags(std::span<const ValueFunction> table) {
  for (const ValueFunction& function : table) {
    for (uint8_t i = 0; i < function.maxArity; i++) {
      if (function.operands[i].unit == ValueFunction::DateUnit::UNSET) return function.name;
      if (function.evalBucketScalar != nullptr && function.operands[i].constant) {
        return function.name;
      }
    }
  }
  return {};
}

} // namespace luxir::valuefn
