#include "luxir/value/ValueExpr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <fmt/format.h>

#include "luxir/reader/FieldReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/value/AggregateExpr.h"

namespace luxir {
namespace {

void requireNumeric(std::span<const ResolvedValue> args) {
  for (const ResolvedValue& arg : args) {
    if (arg.type == ValueType::COLUMN_ONLY) {
      throw std::runtime_error(
          "a non-numeric column is only valid as the complete sort expression");
    }
  }
}

ResolvedValue unaryDemote(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  return {args[0].type, ValueNature::NUMBER};
}

ResolvedValue unaryDouble(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  return {valueArray(args[0].type) ? ValueType::DOUBLE_ARRAY : ValueType::DOUBLE,
          ValueNature::NUMBER};
}

ValueType binaryNumericType(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  if (valueArray(args[0].type) && valueArray(args[1].type)) {
    throw std::runtime_error(fmt::format(
        "array-to-array arithmetic is not implicit; reduce one side with {}",
        ValueFunctionRegistry::arrayReducerNames()));
  }
  bool array = valueArray(args[0].type) || valueArray(args[1].type);
  bool floating = valueDouble(args[0].type) || valueDouble(args[1].type);
  if (array) return floating ? ValueType::DOUBLE_ARRAY : ValueType::INT64_ARRAY;
  return floating ? ValueType::DOUBLE : ValueType::INT64;
}

ResolvedValue addResolve(std::span<const ResolvedValue> args) {
  ValueType type = binaryNumericType(args);
  int dates = (int)std::ranges::count(args, ValueNature::DATE,
                                      &ResolvedValue::nature);
  if (dates == 2) throw std::runtime_error("cannot add two DATE values");
  return {type, dates == 1 ? ValueNature::DATE : ValueNature::NUMBER};
}

ResolvedValue subResolve(std::span<const ResolvedValue> args) {
  ValueType type = binaryNumericType(args);
  if (args[0].nature == ValueNature::NUMBER
      && args[1].nature == ValueNature::DATE) {
    throw std::runtime_error("cannot subtract a DATE from a number");
  }
  if (args[0].nature == ValueNature::DATE
      && args[1].nature == ValueNature::NUMBER) {
    return {type, ValueNature::DATE};
  }
  return {type, ValueNature::NUMBER};
}

ResolvedValue mulResolve(std::span<const ResolvedValue> args) {
  return {binaryNumericType(args), ValueNature::NUMBER};
}

ResolvedValue divResolve(std::span<const ResolvedValue> args) {
  ValueType type = binaryNumericType(args);
  return {valueArray(type) ? ValueType::DOUBLE_ARRAY : ValueType::DOUBLE,
          ValueNature::NUMBER};
}

ResolvedValue defResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  if (valueArray(args[0].type) != valueArray(args[1].type)) {
    throw std::runtime_error("both arguments must be scalars or both must be arrays");
  }
  bool floating = valueDouble(args[0].type) || valueDouble(args[1].type);
  ValueType type = valueArray(args[0].type)
      ? (floating ? ValueType::DOUBLE_ARRAY : ValueType::INT64_ARRAY)
      : (floating ? ValueType::DOUBLE : ValueType::INT64);
  ValueNature nature = std::ranges::find(args, ValueNature::DATE,
                                         &ResolvedValue::nature) != args.end()
      ? ValueNature::DATE : ValueNature::NUMBER;
  return {type, nature};
}

ResolvedValue minMaxResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  if (args.size() == 1) {
    if (!valueArray(args[0].type)) {
      throw std::runtime_error("the one-argument form requires an array");
    }
    return {valueScalarType(args[0].type), args[0].nature};
  }
  if (args[0].nature != args[1].nature) {
    throw std::runtime_error(
        "cannot compare a DATE and a number with min() or max()");
  }
  return {binaryNumericType(args), args[0].nature};
}

ResolvedValue avgResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  if (!valueArray(args[0].type)) {
    throw std::runtime_error("avg() requires a numeric array");
  }
  return {ValueType::DOUBLE, args[0].nature};
}

ResolvedValue sumResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  if (!valueArray(args[0].type)) {
    throw std::runtime_error("sum() requires a numeric array");
  }
  if (args[0].nature == ValueNature::DATE) {
    throw std::runtime_error("sum() cannot reduce a DATE array");
  }
  return {valueScalarType(args[0].type), ValueNature::NUMBER};
}

ResolvedValue countResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  return {ValueType::INT64, ValueNature::NUMBER};
}

[[noreturn]] void runtimeInvalid(const ValueNode& node, int32_t docid,
                                 std::string_view reason) {
  throw ValueEvaluationError(fmt::format(
      "value function {}() {} at segment doc {}", node.text, reason, docid));
}

ValueResult promote(ValueResult value, ValueType type) {
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

enum class ScalarKernelFailure : uint8_t {
  NONE,
  INTEGER_OVERFLOW,
  NON_FINITE,
};

template <class Int>
struct ScalarKernelValue {
  bool valid = false;
  bool floating = false;
  ScalarKernelFailure failure = ScalarKernelFailure::NONE;
  Int intValue = 0;
  double doubleValue = 0.0;
};

template <class Int>
ScalarKernelValue<Int> integerKernelValue(Int value) {
  ScalarKernelValue<Int> result;
  result.valid = true;
  result.intValue = value;
  return result;
}

template <class Int>
ScalarKernelValue<Int> floatingKernelValue(double value) {
  ScalarKernelValue<Int> result;
  result.valid = true;
  result.floating = true;
  result.doubleValue = value;
  return result;
}

template <class Int>
ScalarKernelValue<Int> failedKernelValue(ScalarKernelFailure failure,
                                         bool floating) {
  ScalarKernelValue<Int> result;
  result.floating = floating;
  result.failure = failure;
  return result;
}

template <class Int>
double kernelDouble(const ScalarKernelValue<Int>& value) {
  return value.floating ? value.doubleValue : (double)value.intValue;
}

template <class Int>
ScalarKernelValue<Int> evalUnaryKernel(
    ValueOpcode opcode, const ScalarKernelValue<Int>& input,
    bool outputDouble) {
  if (!input.valid) return {};
  if (outputDouble) {
    double value = kernelDouble(input);
    double output = 0.0;
    switch (opcode) {
      case ValueOpcode::NEG: output = -value; break;
      case ValueOpcode::ABS: output = std::abs(value); break;
      case ValueOpcode::SQRT: output = std::sqrt(value); break;
      case ValueOpcode::LOG: output = std::log(value); break;
      case ValueOpcode::LOG1P: output = std::log1p(value); break;
      case ValueOpcode::FLOOR: output = std::floor(value); break;
      case ValueOpcode::NONE:
      case ValueOpcode::DEF:
      case ValueOpcode::ADD:
      case ValueOpcode::SUB:
      case ValueOpcode::MUL:
      case ValueOpcode::DIV:
      case ValueOpcode::MIN:
      case ValueOpcode::MAX:
      case ValueOpcode::AVG:
        throw std::logic_error("invalid unary ValueExpr opcode");
    }
    if (!std::isfinite(output)
        && opcode != ValueOpcode::MIN && opcode != ValueOpcode::MAX) {
      return failedKernelValue<Int>(ScalarKernelFailure::NON_FINITE, true);
    }
    return floatingKernelValue<Int>(output);
  }

  Int output = 0;
  bool overflow = false;
  switch (opcode) {
    case ValueOpcode::NEG:
      overflow = __builtin_sub_overflow((Int)0, input.intValue, &output);
      break;
    case ValueOpcode::ABS:
      if (input.intValue >= 0) return integerKernelValue(input.intValue);
      overflow = __builtin_sub_overflow((Int)0, input.intValue, &output);
      break;
    case ValueOpcode::NONE:
    case ValueOpcode::DEF:
    case ValueOpcode::ADD:
    case ValueOpcode::SUB:
    case ValueOpcode::MUL:
    case ValueOpcode::DIV:
    case ValueOpcode::SQRT:
    case ValueOpcode::LOG:
    case ValueOpcode::LOG1P:
    case ValueOpcode::FLOOR:
    case ValueOpcode::MIN:
    case ValueOpcode::MAX:
    case ValueOpcode::AVG:
      throw std::logic_error("invalid integer unary ValueExpr opcode");
  }
  return overflow
      ? failedKernelValue<Int>(ScalarKernelFailure::INTEGER_OVERFLOW, false)
      : integerKernelValue(output);
}

template <class Int>
ScalarKernelValue<Int> evalBinaryKernel(
    ValueOpcode opcode, const ScalarKernelValue<Int>& left,
    const ScalarKernelValue<Int>& right, bool outputDouble) {
  if (!left.valid || !right.valid) return {};
  if (outputDouble) {
    double a = kernelDouble(left);
    double b = kernelDouble(right);
    if (opcode == ValueOpcode::DIV && b == 0.0) return {};
    double output = 0.0;
    switch (opcode) {
      case ValueOpcode::ADD: output = a + b; break;
      case ValueOpcode::SUB: output = a - b; break;
      case ValueOpcode::MUL: output = a * b; break;
      case ValueOpcode::DIV: output = a / b; break;
      case ValueOpcode::MIN: output = std::min(a, b); break;
      case ValueOpcode::MAX: output = std::max(a, b); break;
      case ValueOpcode::NONE:
      case ValueOpcode::DEF:
      case ValueOpcode::NEG:
      case ValueOpcode::ABS:
      case ValueOpcode::SQRT:
      case ValueOpcode::LOG:
      case ValueOpcode::LOG1P:
      case ValueOpcode::FLOOR:
      case ValueOpcode::AVG:
        throw std::logic_error("invalid binary ValueExpr opcode");
    }
    if (!std::isfinite(output)
        && opcode != ValueOpcode::MIN && opcode != ValueOpcode::MAX) {
      return failedKernelValue<Int>(ScalarKernelFailure::NON_FINITE, true);
    }
    return floatingKernelValue<Int>(output);
  }

  Int output = 0;
  bool overflow = false;
  switch (opcode) {
    case ValueOpcode::ADD:
      overflow = __builtin_add_overflow(left.intValue, right.intValue,
                                        &output);
      break;
    case ValueOpcode::SUB:
      overflow = __builtin_sub_overflow(left.intValue, right.intValue,
                                        &output);
      break;
    case ValueOpcode::MUL:
      overflow = __builtin_mul_overflow(left.intValue, right.intValue,
                                        &output);
      break;
    case ValueOpcode::MIN:
      output = std::min(left.intValue, right.intValue);
      break;
    case ValueOpcode::MAX:
      output = std::max(left.intValue, right.intValue);
      break;
    case ValueOpcode::NONE:
    case ValueOpcode::DEF:
    case ValueOpcode::DIV:
    case ValueOpcode::NEG:
    case ValueOpcode::ABS:
    case ValueOpcode::SQRT:
    case ValueOpcode::LOG:
    case ValueOpcode::LOG1P:
    case ValueOpcode::FLOOR:
    case ValueOpcode::AVG:
      throw std::logic_error("invalid integer binary ValueExpr opcode");
  }
  return overflow
      ? failedKernelValue<Int>(ScalarKernelFailure::INTEGER_OVERFLOW, false)
      : integerKernelValue(output);
}

ScalarKernelValue<int64_t> kernelValue(const ValueResult& value) {
  if (!value.valid) return {};
  return value.type == ValueType::DOUBLE
      ? floatingKernelValue<int64_t>(value.doubleValue)
      : integerKernelValue<int64_t>(value.intValue);
}

ValueResult evalBinaryScalar(const ValueNode& node, ValueResult left, ValueResult right,
                             int32_t docid) {
  ScalarKernelValue<int64_t> result = evalBinaryKernel(
      node.opcode, kernelValue(left), kernelValue(right),
      node.type == ValueType::DOUBLE);
  if (result.failure == ScalarKernelFailure::INTEGER_OVERFLOW) {
    runtimeInvalid(node, docid, "overflowed int64");
  }
  if (result.failure == ScalarKernelFailure::NON_FINITE) {
    runtimeInvalid(node, docid, "produced NaN or infinity");
  }
  if (!result.valid) return ValueResult::missing(node.type);
  return result.floating ? ValueResult::floating(result.doubleValue)
                         : ValueResult::integer(result.intValue);
}

ValueResult evalUnaryScalar(const ValueNode& node, ValueResult input,
                            int32_t docid) {
  ScalarKernelValue<int64_t> result = evalUnaryKernel(
      node.opcode, kernelValue(input), node.type == ValueType::DOUBLE);
  if (result.failure == ScalarKernelFailure::INTEGER_OVERFLOW) {
    runtimeInvalid(node, docid, "overflowed int64");
  }
  if (result.failure == ScalarKernelFailure::NON_FINITE) {
    runtimeInvalid(node, docid, "produced NaN or infinity");
  }
  if (!result.valid) return ValueResult::missing(node.type);
  return result.floating ? ValueResult::floating(result.doubleValue)
                         : ValueResult::integer(result.intValue);
}

ScalarKernelValue<__int128> kernelValue(const BucketScalar& value) {
  if (!value.valid) return {};
  return value.type == BucketValueType::DOUBLE
      ? floatingKernelValue<__int128>(value.doubleValue)
      : integerKernelValue<__int128>(value.intValue);
}

template <ValueOpcode Opcode, size_t Arity>
BucketScalar evalBucketOperation(
    std::span<const BucketScalar> args, ValueType type, ValueNature nature) {
  static_assert(Arity == 1 || Arity == 2);
  assert(args.size() == Arity);
  BucketValueType bucketType = type == ValueType::DOUBLE
      ? BucketValueType::DOUBLE : BucketValueType::INT128;
  for (const BucketScalar& arg : args) {
    if (arg.failure != AggregateFailure::NONE) {
      return BucketScalar::failed(bucketType, nature, arg.failure);
    }
  }
  ScalarKernelValue<__int128> result;
  if constexpr (Arity == 1) {
    result = evalUnaryKernel(
        Opcode, kernelValue(args[0]), type == ValueType::DOUBLE);
  } else {
    result = evalBinaryKernel(
        Opcode, kernelValue(args[0]), kernelValue(args[1]),
        type == ValueType::DOUBLE);
  }
  if (result.failure == ScalarKernelFailure::INTEGER_OVERFLOW) {
    return BucketScalar::failed(bucketType, nature,
                                AggregateFailure::INT128_OVERFLOW);
  }
  if (result.failure == ScalarKernelFailure::NON_FINITE) {
    return BucketScalar::failed(bucketType, nature,
                                AggregateFailure::NON_FINITE_EXPRESSION);
  }
  if (!result.valid) return BucketScalar::missing(bucketType, nature);
  return result.floating ? BucketScalar::floating(result.doubleValue, nature)
                         : BucketScalar::integer(result.intValue, nature);
}

bool unaryOpcode(ValueOpcode opcode) {
  return opcode == ValueOpcode::NEG || opcode == ValueOpcode::ABS
      || opcode == ValueOpcode::SQRT || opcode == ValueOpcode::LOG
      || opcode == ValueOpcode::LOG1P || opcode == ValueOpcode::FLOOR;
}

bool reducerOpcode(ValueOpcode opcode) {
  return opcode == ValueOpcode::MIN || opcode == ValueOpcode::MAX
      || opcode == ValueOpcode::AVG;
}

ValueResult evalSumPoint(BoundValueProgram& program, const ValueNode& node,
                         int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid || input.array.size == 0) {
    return ValueResult::missing(node.type);
  }
  int64_t intSum = 0;
  double doubleSum = 0.0;
  int64_t present = 0;
  for (int64_t i = 0; i < input.array.size; i++) {
    ValueResult value = program.evalArrayElement(input.array, i);
    if (!value.valid) continue;
    if (node.type == ValueType::DOUBLE) {
      double addend = value.type == ValueType::DOUBLE
          ? value.doubleValue : (double)value.intValue;
      double next = doubleSum + addend;
      if (!std::isfinite(next)) return ValueResult::missing(node.type);
      doubleSum = next;
    } else {
      int64_t next;
      if (__builtin_add_overflow(intSum, value.intValue, &next)) {
        return ValueResult::missing(node.type);
      }
      intSum = next;
    }
    present++;
  }
  if (present == 0) return ValueResult::missing(node.type);
  return node.type == ValueType::DOUBLE ? ValueResult::floating(doubleSum)
                                        : ValueResult::integer(intSum);
}

ValueResult evalCountPoint(BoundValueProgram& program, const ValueNode& node,
                           int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid) return ValueResult::integer(0);
  if (!valueArray(input.type)) return ValueResult::integer(1);
  int64_t present = 0;
  for (int64_t i = 0; i < input.array.size; i++) {
    if (program.evalArrayElement(input.array, i).valid) present++;
  }
  return ValueResult::integer(present);
}

ValueResult invalidReducerElement(
    BoundValueProgram& program, const ValueNode& node,
    const ValueArrayRef& array, int64_t index) {
  unused(program);
  unused(node);
  unused(array);
  unused(index);
  throw std::logic_error("scalar reducer used as an array expression");
}

ValueResult evalSumElement(BoundValueProgram& program, const ValueNode& node,
                           const ValueArrayRef& array, int64_t index) {
  return invalidReducerElement(program, node, array, index);
}

ValueResult evalCountElement(BoundValueProgram& program, const ValueNode& node,
                             const ValueArrayRef& array, int64_t index) {
  return invalidReducerElement(program, node, array, index);
}

ValueResult evalFunctionPoint(BoundValueProgram& program, const ValueNode& node,
                              int32_t docid, float score) {
  ValueResult first = program.evalNode(node.children[0], docid, score);
  if (node.opcode == ValueOpcode::DEF) {
    ValueResult selected = first.valid ? first : program.evalNode(node.children[1], docid, score);
    if (selected.valid && valueArray(node.type)) {
      return ValueResult::arrayValue(
          node.type, (uint32_t)(&node - program.program.nodes.data()), docid, score,
          selected.array.size);
    }
    return promote(selected, node.type);
  }

  if (reducerOpcode(node.opcode)) {
    bool reducer = node.childCount == 1;
    if (reducer) {
      if (!first.valid || first.array.size == 0) return ValueResult::missing(node.type);
      ValueResult aggregate = ValueResult::missing(valueScalarType(first.type));
      long double sum = 0.0;
      int64_t present = 0;
      for (int64_t i = 0; i < first.array.size; i++) {
        ValueResult next = program.evalArrayElement(first.array, i);
        if (!next.valid) continue;
        if (node.opcode == ValueOpcode::AVG) {
          sum += next.type == ValueType::DOUBLE ? next.doubleValue : next.intValue;
        } else if (!aggregate.valid) {
          aggregate = next;
        } else if (node.type == ValueType::DOUBLE) {
          double a = aggregate.type == ValueType::DOUBLE ? aggregate.doubleValue
                                                        : (double)aggregate.intValue;
          double b = next.type == ValueType::DOUBLE ? next.doubleValue : (double)next.intValue;
          aggregate = ValueResult::floating(
              node.opcode == ValueOpcode::MIN ? std::min(a, b) : std::max(a, b));
        } else {
          aggregate.intValue = node.opcode == ValueOpcode::MIN
              ? std::min(aggregate.intValue, next.intValue)
              : std::max(aggregate.intValue, next.intValue);
        }
        present++;
      }
      if (present == 0) return ValueResult::missing(node.type);
      if (node.opcode == ValueOpcode::AVG) {
        double out = (double)(sum / present);
        if (!std::isfinite(out)) runtimeInvalid(node, docid, "produced NaN or infinity");
        return ValueResult::floating(out);
      }
      return promote(aggregate, node.type);
    }
  }

  if (unaryOpcode(node.opcode)) {
    if (!first.valid) return ValueResult::missing(node.type);
    if (valueArray(first.type)) {
      return ValueResult::arrayValue(node.type, (uint32_t)(&node - program.program.nodes.data()),
                                     docid, score, first.array.size);
    }
    return evalUnaryScalar(node, first, docid);
  }

  ValueResult second = program.evalNode(node.children[1], docid, score);
  if (valueArray(node.type)) {
    if (!first.valid || !second.valid) return ValueResult::missing(node.type);
    int64_t size = valueArray(first.type) ? first.array.size : second.array.size;
    return ValueResult::arrayValue(node.type, (uint32_t)(&node - program.program.nodes.data()),
                                   docid, score, size);
  }
  return evalBinaryScalar(node, first, second, docid);
}

void evalFunctionBatch(BoundValueProgram& program, const ValueNode& node,
                       std::span<const int32_t> docids, std::span<const float> scores,
                       std::span<ValueResult> results) {
  for (size_t i = 0; i < docids.size(); i++) {
    float score = scores.empty() ? 0.0f : scores[i];
    results[i] = node.function->evalPoint(program, node, docids[i], score);
  }
}

ValueResult evalFunctionElement(BoundValueProgram& program, const ValueNode& node,
                                const ValueArrayRef& array, int64_t index) {
  auto operand = [&](uint32_t child) {
    ValueResult value = program.evalNode(child, array.docid, array.score);
    return valueArray(value.type) ? program.evalArrayElement(value.array, index) : value;
  };
  ValueResult first = operand(node.children[0]);
  if (node.opcode == ValueOpcode::DEF) {
    if (first.valid) return promote(first, valueScalarType(node.type));
    return promote(operand(node.children[1]), valueScalarType(node.type));
  }
  if (unaryOpcode(node.opcode)) {
    ValueNode scalar = node;
    scalar.type = valueScalarType(node.type);
    return evalUnaryScalar(scalar, first, array.docid);
  }
  ValueResult second = operand(node.children[1]);
  ValueNode scalar = node;
  scalar.type = valueScalarType(node.type);
  return evalBinaryScalar(scalar, first, second, array.docid);
}

bool invalid(const ValueBounds& bounds) {
  return bounds.certainty == BoundsCertainty::INVALID;
}

bool bounded(const ValueBounds& bounds) {
  return bounds.certainty == BoundsCertainty::BOUNDED;
}

ValueBounds retag(ValueBounds bounds, ValueType type) {
  bounds.type = type;
  return bounds;
}

ValueBounds promoteBounds(ValueBounds bounds, ValueType type) {
  if (valueDouble(type) && !valueDouble(bounds.type)
      && bounded(bounds)) {
    ValueBounds promoted = ValueBounds::floating(
        (double)bounds.intMin, (double)bounds.intMax, bounds.mayBeMissing);
    promoted.type = type;
    promoted.alwaysMissing = bounds.alwaysMissing;
    promoted.minAttained = bounds.minAttained;
    promoted.maxAttained = bounds.maxAttained;
    return promoted;
  }
  return retag(bounds, type);
}

ValueBounds unaryBounds(const ValueNode& node, const ValueBounds& input) {
  if (invalid(input)) return retag(input, node.type);
  if (!bounded(input)) {
    return ValueBounds::unbounded(node.type, input.mayBeMissing, input.alwaysMissing);
  }
  if (input.alwaysMissing) {
    return ValueBounds::unbounded(node.type, true, true);
  }

  double low = boundMinAsDouble(input);
  double high = boundMaxAsDouble(input);
  if ((node.opcode == ValueOpcode::NEG || node.opcode == ValueOpcode::ABS)
      && !valueDouble(node.type)) {
    if (input.intMin == std::numeric_limits<int64_t>::min() && input.minAttained) {
      return ValueBounds::invalid(node.type, BoundsInvalidity::INTEGER_OVERFLOW,
                                  input.mayBeMissing);
    }
    ValueBounds out;
    if (node.opcode == ValueOpcode::NEG) {
      out = ValueBounds::integer(-input.intMax, -input.intMin,
                                 input.mayBeMissing);
    }
    else if (input.intMin >= 0) out = ValueBounds::integer(input.intMin, input.intMax,
                                                           input.mayBeMissing);
    else if (input.intMax <= 0) out = ValueBounds::integer(-input.intMax, -input.intMin,
                                                           input.mayBeMissing);
    else out = ValueBounds::integer(0, std::max(-input.intMin, input.intMax),
                                    input.mayBeMissing);
    out.type = node.type;
    if (node.opcode == ValueOpcode::NEG) {
      out.minAttained = input.maxAttained;
      out.maxAttained = input.minAttained;
    } else if (input.intMin < 0 && input.intMax > 0) {
      out.minAttained = false;
      out.maxAttained = input.minAttained || input.maxAttained;
    }
    return out;
  }

  if (node.opcode == ValueOpcode::SQRT && low < 0.0 && input.minAttained) {
    return ValueBounds::invalid(node.type, BoundsInvalidity::DOMAIN, input.mayBeMissing);
  }
  if (node.opcode == ValueOpcode::LOG && low <= 0.0 && input.minAttained) {
    return ValueBounds::invalid(node.type, BoundsInvalidity::DOMAIN, input.mayBeMissing);
  }
  if (node.opcode == ValueOpcode::LOG1P && low <= -1.0 && input.minAttained) {
    return ValueBounds::invalid(node.type, BoundsInvalidity::DOMAIN, input.mayBeMissing);
  }
  if ((node.opcode == ValueOpcode::SQRT && low < 0.0)
      || (node.opcode == ValueOpcode::LOG && low <= 0.0)
      || (node.opcode == ValueOpcode::LOG1P && low <= -1.0)) {
    return ValueBounds::unbounded(node.type, input.mayBeMissing);
  }

  double outLow;
  double outHigh;
  switch (node.opcode) {
    case ValueOpcode::NEG:
      outLow = -high;
      outHigh = -low;
      break;
    case ValueOpcode::ABS:
      outLow = low <= 0.0 && high >= 0.0
          ? 0.0 : std::min(std::abs(low), std::abs(high));
      outHigh = std::max(std::abs(low), std::abs(high));
      break;
    case ValueOpcode::SQRT:
      outLow = std::sqrt(low);
      outHigh = std::sqrt(high);
      break;
    case ValueOpcode::LOG:
      outLow = std::log(low);
      outHigh = std::log(high);
      break;
    case ValueOpcode::LOG1P:
      outLow = std::log1p(low);
      outHigh = std::log1p(high);
      break;
    case ValueOpcode::FLOOR:
      outLow = std::floor(low);
      outHigh = std::floor(high);
      break;
    default:
      throw std::runtime_error("invalid unary ValueExpr opcode");
  }
  if (!std::isfinite(outLow) || !std::isfinite(outHigh)) {
    BoundsInvalidity why = std::isnan(outLow) || std::isnan(outHigh)
        ? BoundsInvalidity::NAN_VALUE
        : outLow < 0.0 ? BoundsInvalidity::NEGATIVE_INFINITY
                       : BoundsInvalidity::POSITIVE_INFINITY;
    if (input.minAttained && input.maxAttained) {
      return ValueBounds::invalid(node.type, why, input.mayBeMissing);
    }
    return ValueBounds::unbounded(node.type, input.mayBeMissing);
  }
  ValueBounds out = ValueBounds::floating(outLow, outHigh, input.mayBeMissing);
  out.type = node.type;
  if (node.opcode == ValueOpcode::NEG) {
    out.minAttained = input.maxAttained;
    out.maxAttained = input.minAttained;
  } else if (node.opcode == ValueOpcode::ABS && low < 0.0 && high > 0.0) {
    out.minAttained = false;
    out.maxAttained = input.minAttained || input.maxAttained;
  } else {
    out.minAttained = input.minAttained;
    out.maxAttained = input.maxAttained;
  }
  return out;
}

ValueBounds binaryBounds(const ValueNode& node, const ValueBounds& left,
                         const ValueBounds& right) {
  if (invalid(left)) return retag(left, node.type);
  if (invalid(right)) return retag(right, node.type);
  bool mayMissing = left.mayBeMissing || right.mayBeMissing;
  bool alwaysMissing = left.alwaysMissing || right.alwaysMissing;
  if (alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
  if (!bounded(left) || !bounded(right)) return ValueBounds::unbounded(node.type, mayMissing);

  if (!valueDouble(node.type)) {
    int64_t amin = left.intMin;
    int64_t amax = left.intMax;
    int64_t bmin = right.intMin;
    int64_t bmax = right.intMax;
    __int128 low = 0;
    __int128 high = 0;
    switch (node.opcode) {
      case ValueOpcode::ADD:
        low = (__int128)amin + bmin;
        high = (__int128)amax + bmax;
        break;
      case ValueOpcode::SUB:
        low = (__int128)amin - bmax;
        high = (__int128)amax - bmin;
        break;
      case ValueOpcode::MUL: {
        std::array<__int128, 4> products{
            (__int128)amin * bmin, (__int128)amin * bmax,
            (__int128)amax * bmin, (__int128)amax * bmax};
        low = *std::min_element(products.begin(), products.end());
        high = *std::max_element(products.begin(), products.end());
        break;
      }
      case ValueOpcode::MIN:
        low = std::min(amin, bmin);
        high = std::min(amax, bmax);
        break;
      case ValueOpcode::MAX:
        low = std::max(amin, bmin);
        high = std::max(amax, bmax);
        break;
      default:
        throw std::runtime_error("invalid binary ValueExpr opcode");
    }
    if (low < std::numeric_limits<int64_t>::min() ||
        high > std::numeric_limits<int64_t>::max()) {
      bool exact = amin == amax && bmin == bmax && left.minAttained && right.minAttained;
      return exact ? ValueBounds::invalid(node.type, BoundsInvalidity::INTEGER_OVERFLOW, mayMissing)
                   : ValueBounds::unbounded(node.type, mayMissing);
    }
    ValueBounds out = ValueBounds::integer((int64_t)low, (int64_t)high, mayMissing);
    out.type = node.type;
    bool leftFixed = amin == amax && !left.mayBeMissing;
    bool rightFixed = bmin == bmax && !right.mayBeMissing;
    out.minAttained = left.minAttained && right.minAttained && (leftFixed || rightFixed);
    out.maxAttained = left.maxAttained && right.maxAttained && (leftFixed || rightFixed);
    return out;
  }

  double amin = boundMinAsDouble(left);
  double amax = boundMaxAsDouble(left);
  double bmin = boundMinAsDouble(right);
  double bmax = boundMaxAsDouble(right);
  double low;
  double high;
  switch (node.opcode) {
    case ValueOpcode::ADD:
      low = amin + bmin;
      high = amax + bmax;
      break;
    case ValueOpcode::SUB:
      low = amin - bmax;
      high = amax - bmin;
      break;
    case ValueOpcode::MUL: {
      std::array<double, 4> products{
          amin * bmin, amin * bmax, amax * bmin, amax * bmax};
      low = *std::min_element(products.begin(), products.end());
      high = *std::max_element(products.begin(), products.end());
      break;
    }
    case ValueOpcode::DIV: {
      if (bmin <= 0.0 && bmax >= 0.0) {
        if (bmin == 0.0 && bmax == 0.0
            && right.minAttained && right.maxAttained) {
          return ValueBounds::unbounded(node.type, true, true);
        }
        return ValueBounds::unbounded(node.type, true);
      }
      std::array<double, 4> quotients{
          amin / bmin, amin / bmax, amax / bmin, amax / bmax};
      low = *std::min_element(quotients.begin(), quotients.end());
      high = *std::max_element(quotients.begin(), quotients.end());
      break;
    }
    case ValueOpcode::MIN:
      low = std::min(amin, bmin);
      high = std::min(amax, bmax);
      break;
    case ValueOpcode::MAX:
      low = std::max(amin, bmin);
      high = std::max(amax, bmax);
      break;
    default:
      throw std::runtime_error("invalid binary ValueExpr opcode");
  }
  if ((!std::isfinite(low) || !std::isfinite(high))
      && node.opcode != ValueOpcode::MIN
      && node.opcode != ValueOpcode::MAX) {
    bool exact = amin == amax && bmin == bmax && left.minAttained && right.minAttained;
    return exact ? ValueBounds::invalid(node.type, BoundsInvalidity::POSITIVE_INFINITY, mayMissing)
                 : ValueBounds::unbounded(node.type, mayMissing);
  }
  ValueBounds out = ValueBounds::floating(low, high, mayMissing);
  out.type = node.type;
  bool leftFixed = amin == amax && !left.mayBeMissing;
  bool rightFixed = bmin == bmax && !right.mayBeMissing;
  out.minAttained = left.minAttained && right.minAttained && (leftFixed || rightFixed);
  out.maxAttained = left.maxAttained && right.maxAttained && (leftFixed || rightFixed);
  return out;
}

ValueBounds propagateBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  if (node.opcode == ValueOpcode::DEF) {
    const ValueBounds& first = args[0];
    const ValueBounds& second = args[1];
    if (invalid(first)) return retag(first, node.type);
    if (first.alwaysMissing) return promoteBounds(second, node.type);
    if (!first.mayBeMissing) return promoteBounds(first, node.type);
    if (invalid(second)) return retag(second, node.type);
    bool mayMissing = first.mayBeMissing && second.mayBeMissing;
    bool alwaysMissing = first.alwaysMissing && second.alwaysMissing;
    if (!bounded(first) || !bounded(second)) {
      return ValueBounds::unbounded(node.type, mayMissing, alwaysMissing);
    }
    if (valueDouble(node.type)) {
      ValueBounds out = ValueBounds::floating(
          std::min(boundMinAsDouble(first), boundMinAsDouble(second)),
          std::max(boundMaxAsDouble(first), boundMaxAsDouble(second)), mayMissing);
      out.type = node.type;
      return out;
    }
    ValueBounds out = ValueBounds::integer(std::min(first.intMin, second.intMin),
                                           std::max(first.intMax, second.intMax), mayMissing);
    out.type = node.type;
    return out;
  }
  if (unaryOpcode(node.opcode)) {
    return unaryBounds(node, args[0]);
  }
  if (reducerOpcode(node.opcode) && node.childCount == 1) {
    ValueBounds out = args[0];
    out.type = node.type;
    if (node.opcode == ValueOpcode::AVG) {
      if (bounded(out)) {
        out.doubleMin = boundMinAsDouble(args[0]);
        out.doubleMax = boundMaxAsDouble(args[0]);
      }
      out.type = ValueType::DOUBLE;
    }
    return out;
  }
  return binaryBounds(node, args[0], args[1]);
}

ValueBounds sumBounds(const ValueNode& node,
                      std::span<const ValueBounds> args) {
  if (invalid(args[0])) return retag(args[0], node.type);
  return ValueBounds::unbounded(node.type, true, args[0].alwaysMissing);
}

ValueBounds countBounds(const ValueNode& node,
                        std::span<const ValueBounds> args) {
  if (invalid(args[0])) return retag(args[0], node.type);
  if (args[0].alwaysMissing) return ValueBounds::integer(0, 0);
  if (!valueArray(args[0].type)) {
    if (!args[0].mayBeMissing) return ValueBounds::integer(1, 1);
    ValueBounds out = ValueBounds::integer(0, 1);
    out.minAttained = false;
    out.maxAttained = false;
    return out;
  }
  ValueBounds out = ValueBounds::integer(
      0, std::numeric_limits<int64_t>::max());
  out.minAttained = false;
  out.maxAttained = false;
  return out;
}

constexpr ValueFunction FUNCTIONS[] = {
    {ValueOpcode::DEF, "def", defResolve, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 2, 2, nullptr},
    {ValueOpcode::ADD, "add", addResolve, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 2, 2,
     evalBucketOperation<ValueOpcode::ADD, 2>},
    {ValueOpcode::SUB, "sub", subResolve, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 2, 2,
     evalBucketOperation<ValueOpcode::SUB, 2>},
    {ValueOpcode::MUL, "mul", mulResolve, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 2, 2,
     evalBucketOperation<ValueOpcode::MUL, 2>},
    {ValueOpcode::DIV, "div", divResolve, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 2, 2,
     evalBucketOperation<ValueOpcode::DIV, 2>},
    {ValueOpcode::NEG, "neg", unaryDemote, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 1,
     evalBucketOperation<ValueOpcode::NEG, 1>},
    {ValueOpcode::ABS, "abs", unaryDemote, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 1,
     evalBucketOperation<ValueOpcode::ABS, 1>},
    {ValueOpcode::SQRT, "sqrt", unaryDouble, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 1,
     evalBucketOperation<ValueOpcode::SQRT, 1>},
    {ValueOpcode::LOG, "log", unaryDouble, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 1,
     evalBucketOperation<ValueOpcode::LOG, 1>},
    {ValueOpcode::LOG1P, "log1p", unaryDouble, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 1,
     evalBucketOperation<ValueOpcode::LOG1P, 1>},
    {ValueOpcode::FLOOR, "floor", unaryDouble, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 1,
     evalBucketOperation<ValueOpcode::FLOOR, 1>},
    {ValueOpcode::MIN, "min", minMaxResolve, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 2,
     evalBucketOperation<ValueOpcode::MIN, 2>},
    {ValueOpcode::MAX, "max", minMaxResolve, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 2,
     evalBucketOperation<ValueOpcode::MAX, 2>},
    {ValueOpcode::AVG, "avg", avgResolve, evalFunctionPoint, evalFunctionBatch,
     propagateBounds, evalFunctionElement, 1, 1, nullptr},
    {ValueOpcode::NONE, "sum", sumResolve, evalSumPoint, evalFunctionBatch,
     sumBounds, evalSumElement, 1, 1, nullptr},
    {ValueOpcode::NONE, "count", countResolve, evalCountPoint,
     evalFunctionBatch, countBounds, evalCountElement, 1, 1, nullptr},
};

ValueBounds constantBounds(const ValueProgram& program, uint32_t index) {
  const ValueNode& node = program.nodes[index];
  if (node.type == ValueType::INT64) return ValueBounds::integer(node.intValue, node.intValue);
  if (node.type == ValueType::DOUBLE) return ValueBounds::floating(node.doubleValue, node.doubleValue);
  if (node.arraySize == 0) {
    ValueBounds out = ValueBounds::unbounded(node.type, true, true);
    return out;
  }
  if (node.type == ValueType::INT64_ARRAY) {
    auto begin = program.intArrays.begin() + node.arrayOffset;
    auto [min, max] = std::minmax_element(begin, begin + node.arraySize);
    ValueBounds out = ValueBounds::integer(*min, *max);
    out.type = node.type;
    return out;
  }
  auto begin = program.doubleArrays.begin() + node.arrayOffset;
  auto [min, max] = std::minmax_element(begin, begin + node.arraySize);
  ValueBounds out = ValueBounds::floating(*min, *max);
  out.type = node.type;
  return out;
}

[[noreturn]] void throwBindInvalid(const ValueProgram& program, const ValueNode& node,
                                   const ValueBounds& bounds) {
  if (node.kind == ValueNodeKind::FUNCTION && node.opcode == ValueOpcode::LOG) {
    std::string_view argument = program.nodes[node.children[0]].text;
    throw ValueEvaluationError(fmt::format(
        "log() is invalid over the segment bounds of '{}'; use log(max({}, 1)) to clamp "
        "or log1p({}) when that is the intended transform",
        argument, argument, argument));
  }
  if (node.kind == ValueNodeKind::FUNCTION && node.opcode == ValueOpcode::LOG1P) {
    throw ValueEvaluationError(
        "log1p() is invalid for a value <= -1 in this segment; clamp its argument first");
  }
  if (node.kind == ValueNodeKind::FUNCTION && node.opcode == ValueOpcode::SQRT) {
    throw ValueEvaluationError(
        "sqrt() is invalid for a negative value in this segment; clamp its argument first");
  }
  if (bounds.invalidity == BoundsInvalidity::INTEGER_OVERFLOW) {
    throw ValueEvaluationError(fmt::format(
        "{}() provably overflows int64 in this segment", node.text));
  }
  throw ValueEvaluationError(fmt::format(
      "{}() provably produces NaN or infinity in this segment", node.text));
}

} // namespace

const ValueFunction* ValueFunctionRegistry::find(std::string_view name) {
  for (const ValueFunction& function : FUNCTIONS) {
    if (function.name == name) return &function;
  }
  return nullptr;
}

std::span<const ValueFunction> ValueFunctionRegistry::entries() {
  return FUNCTIONS;
}

std::string ValueFunctionRegistry::arrayReducerNames() {
  std::vector<std::string_view> names;
  std::array<ResolvedValue, 1> array{{
      {ValueType::INT64_ARRAY, ValueNature::NUMBER}}};
  for (const ValueFunction& function : FUNCTIONS) {
    if (function.minArity > 1 || function.maxArity < 1) continue;
    try {
      ResolvedValue resolved = function.resolve(array);
      if (!valueArray(resolved.type)
          && resolved.type != ValueType::COLUMN_ONLY) {
        names.push_back(function.name);
      }
    } catch (const std::exception&) {
    }
  }
  std::string result;
  for (size_t i = 0; i < names.size(); i++) {
    if (i != 0) result += i + 1 == names.size() ? ", or " : ", ";
    result += names[i];
    result += "(...)";
  }
  return result;
}

BoundValueProgram* ValueProgram::bind(MemPool& pool,
                                      IndexReader::Segment& segment) const {
  return pool.make<BoundValueProgram>(pool, *this, segment);
}

BoundValueProgram::BoundValueProgram(MemPool& pool, const ValueProgram& program,
                                     IndexReader::Segment& segment)
    : program(program), segment(segment), postings(segment.postingsReader()),
      nodes(pool.make_span<BoundValueNode>(program.nodes.size())) {
  for (uint32_t index = 0; index < program.nodes.size(); index++) {
    const ValueNode& node = program.nodes[index];
    BoundValueNode& bound = nodes[index];
    if (node.kind == ValueNodeKind::CONSTANT || node.kind == ValueNodeKind::VARIABLE) {
      bound.bounds = constantBounds(program, index);
    } else if (node.kind == ValueNodeKind::SCORE) {
      bound.bounds = ValueBounds::unbounded(ValueType::DOUBLE);
    } else if (node.kind == ValueNodeKind::DOCID) {
      int32_t maxDoc = postings.maxDoc();
      bound.bounds = maxDoc == 0 ? ValueBounds::unbounded(ValueType::INT64, true, true)
                                : ValueBounds::integer(segment.base,
                                                       segment.base + maxDoc - 1);
    } else if (node.kind == ValueNodeKind::COLUMN) {
      FieldReader fields(postings);
      if (!fields.seek(node.text)) {
        bound.bounds = ValueBounds::unbounded(node.type, true, true);
      } else {
        SegFieldInfo info;
        fields.readFieldInfo(info);
        bound.column.emplace(postings, info);
        bound.iterator.emplace(*bound.column);
        bool missing = bound.column->docsWithValue() < postings.maxDoc();
        auto encoded = bound.column->encodedBounds();
        if (!encoded.hasValues) {
          bound.bounds = ValueBounds::unbounded(node.type, true, true);
        } else if (node.columnType == FieldType::FLOAT) {
          double min = (double)sortableInt32ToFloat((int32_t)encoded.min);
          double max = (double)sortableInt32ToFloat((int32_t)encoded.max);
          bound.bounds = ValueBounds::floating(min, max, missing);
          bound.bounds.type = node.type;
        } else if (node.columnType == FieldType::DOUBLE) {
          double min = sortableInt64ToDouble(encoded.min);
          double max = sortableInt64ToDouble(encoded.max);
          bound.bounds = ValueBounds::floating(min, max, missing);
          bound.bounds.type = node.type;
        } else {
          bound.bounds = ValueBounds::integer(encoded.min, encoded.max, missing);
          bound.bounds.type = node.type;
        }
        if (valueDouble(node.type) && bounded(bound.bounds)
            && (std::isnan(bound.bounds.doubleMin)
                || std::isnan(bound.bounds.doubleMax))) {
          // NaN column values evaluate as missing. Encoded endpoint metadata
          // cannot exclude those values to recover the non-NaN interval.
          bound.bounds = ValueBounds::unbounded(node.type, true);
        }
      }
    }
  }
  propagateBounds(ValueBounds::unbounded(ValueType::DOUBLE));
}

void BoundValueProgram::propagateBounds(const ValueBounds& scoreBounds) {
  for (uint32_t index = 0; index < program.nodes.size(); index++) {
    const ValueNode& node = program.nodes[index];
    BoundValueNode& bound = nodes[index];
    if (node.kind == ValueNodeKind::SCORE) {
      bound.bounds = scoreBounds;
      bound.bounds.type = ValueType::DOUBLE;
    } else if (node.kind == ValueNodeKind::FUNCTION) {
      std::array<ValueBounds, 2> children{};
      for (uint8_t child = 0; child < node.childCount; child++) {
        children[child] = nodes[node.children[child]].bounds;
      }
      bound.bounds = node.function->boundsPropagate(
          node, std::span<const ValueBounds>(children.data(), node.childCount));
    }
    if (bound.bounds.certainty == BoundsCertainty::INVALID) {
      throwBindInvalid(program, node, bound.bounds);
    }
  }
  cachedScoreBounds = scoreBounds;
  boundsCached = true;
}

const ValueBounds& BoundValueProgram::boundsForScore(
    const ValueBounds& scoreBounds) {
  if (!boundsCached || cachedScoreBounds != scoreBounds) {
    propagateBounds(scoreBounds);
  }
  return nodes[program.rootNode].bounds;
}

std::string_view BoundValueProgram::firstUnboundedNode() const {
  const ValueBounds& root = nodes[program.rootNode].bounds;
  if (root.certainty == BoundsCertainty::BOUNDED
      && !root.mayBeMissing && !root.alwaysMissing) {
    return {};
  }
  for (uint32_t index = 0; index < program.nodes.size(); index++) {
    const ValueBounds& valueBounds = nodes[index].bounds;
    if (valueBounds.certainty != BoundsCertainty::BOUNDED
        || valueBounds.mayBeMissing || valueBounds.alwaysMissing) {
      return program.nodes[index].text;
    }
  }
  return {};
}

ValueResult BoundValueProgram::evalConstant(uint32_t index, int32_t docid, float score) const {
  const ValueNode& node = program.nodes[index];
  if (node.type == ValueType::INT64) return ValueResult::integer(node.intValue);
  if (node.type == ValueType::DOUBLE) return ValueResult::floating(node.doubleValue);
  return ValueResult::arrayValue(node.type, index, docid, score, node.arraySize);
}

ValueResult BoundValueProgram::evalColumn(uint32_t index, int32_t docid, float score) {
  const ValueNode& node = program.nodes[index];
  BoundValueNode& bound = nodes[index];
  if (!bound.column) return ValueResult::missing(node.type);
  if (bound.cachedDoc != docid) {
    if (!bound.iterator || bound.iterator->docId() > docid) {
      bound.iterator.reset();
      bound.iterator.emplace(*bound.column);
    }
    int32_t found = bound.iterator->docId();
    if (found < docid) found = bound.iterator->advance(docid);
    bound.cachedDoc = docid;
    bound.cachedPresent = found == docid;
    if (bound.cachedPresent) {
      if (node.columnMultiValued && bound.column->multiValued()) {
        auto range = bound.column->getStartEndValueRank(bound.iterator->rank());
        bound.valueStart = range.first;
        bound.valueEnd = range.second;
      } else {
        bound.valueStart = bound.iterator->rank();
        bound.valueEnd = bound.valueStart + 1;
      }
    }
  }
  if (!bound.cachedPresent) return ValueResult::missing(node.type);
  if (valueArray(node.type)) {
    return ValueResult::arrayValue(node.type, index, docid, score,
                                   bound.valueEnd - bound.valueStart);
  }
  int64_t raw = bound.iterator->values().valueAt(bound.valueStart);
  if (node.columnType == FieldType::FLOAT) {
    double decoded = (double)sortableInt32ToFloat((int32_t)raw);
    if (std::isnan(decoded)) return ValueResult::missing(node.type);
    return ValueResult::floating(decoded);
  }
  if (node.columnType == FieldType::DOUBLE) {
    double decoded = sortableInt64ToDouble(raw);
    if (std::isnan(decoded)) return ValueResult::missing(node.type);
    return ValueResult::floating(decoded);
  }
  return ValueResult::integer(raw);
}

ValueResult BoundValueProgram::evalNode(uint32_t index, int32_t docid, float score) {
  const ValueNode& node = program.nodes[index];
  switch (node.kind) {
    case ValueNodeKind::CONSTANT:
    case ValueNodeKind::VARIABLE: return evalConstant(index, docid, score);
    case ValueNodeKind::COLUMN: return evalColumn(index, docid, score);
    case ValueNodeKind::SCORE: return ValueResult::floating((double)score);
    case ValueNodeKind::DOCID: return ValueResult::integer(segment.base + docid);
    case ValueNodeKind::FUNCTION: return node.function->evalPoint(*this, node, docid, score);
  }
  throw std::runtime_error("invalid ValueExpr node");
}

ValueResult BoundValueProgram::evalPoint(int32_t docid, float score) {
  return evalNode(program.rootNode, docid, score);
}

void BoundValueProgram::evalBatch(std::span<const int32_t> docids,
                                  std::span<const float> scores,
                                  std::span<ValueResult> results) {
  if (results.size() != docids.size() || (!scores.empty() && scores.size() != docids.size())) {
    throw std::runtime_error("ValueExpr batch spans have different lengths");
  }
  const ValueNode& root = program.root();
  if (root.kind == ValueNodeKind::FUNCTION) {
    root.function->evalBatch(*this, root, docids, scores, results);
    return;
  }
  for (size_t i = 0; i < docids.size(); i++) {
    results[i] = evalNode(program.rootNode, docids[i], scores.empty() ? 0.0f : scores[i]);
  }
}

ValueResult BoundValueProgram::evalArrayElement(const ValueArrayRef& array, int64_t element) {
  const ValueNode& node = program.nodes[array.node];
  if (element < 0 || element >= array.size) throw std::runtime_error("ValueExpr array index out of range");
  if (node.kind == ValueNodeKind::CONSTANT || node.kind == ValueNodeKind::VARIABLE) {
    if (node.type == ValueType::INT64_ARRAY) {
      return ValueResult::integer(program.intArrays[node.arrayOffset + element]);
    }
    return ValueResult::floating(program.doubleArrays[node.arrayOffset + element]);
  }
  if (node.kind == ValueNodeKind::COLUMN) {
    BoundValueNode& bound = nodes[array.node];
    (void)evalColumn(array.node, array.docid, array.score);
    int64_t raw = bound.iterator->values().valueAt(bound.valueStart + element);
    if (node.columnType == FieldType::FLOAT) {
      double decoded = (double)sortableInt32ToFloat((int32_t)raw);
      if (std::isnan(decoded)) return ValueResult::missing(valueScalarType(node.type));
      return ValueResult::floating(decoded);
    }
    if (node.columnType == FieldType::DOUBLE) {
      double decoded = sortableInt64ToDouble(raw);
      if (std::isnan(decoded)) return ValueResult::missing(valueScalarType(node.type));
      return ValueResult::floating(decoded);
    }
    return ValueResult::integer(raw);
  }
  if (node.kind == ValueNodeKind::FUNCTION) {
    return node.function->evalElement(*this, node, array, element);
  }
  throw std::runtime_error("scalar ValueExpr node used as an array");
}

int64_t BoundValueProgram::arraySize(uint32_t index, int32_t docid, float score) {
  ValueResult result = evalNode(index, docid, score);
  return result.valid && valueArray(result.type) ? result.array.size : 0;
}

} // namespace luxir
