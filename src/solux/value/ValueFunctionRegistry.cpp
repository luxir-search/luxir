#include "solux/value/ValueExpr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <fmt/format.h>

#include "solux/reader/FieldReader.h"
#include "solux/reader/PostingsReader.h"
#include "solux/util/NumericUtils.h"

namespace solux {
namespace {

void requireNumeric(std::span<const ValueType> args) {
  for (ValueType type : args) {
    if (type == ValueType::COLUMN_ONLY) {
      throw std::runtime_error(
          "a non-numeric column is only valid as the complete sort expression");
    }
  }
}

ValueType unarySame(std::span<const ValueType> args) {
  requireNumeric(args);
  return args[0];
}

ValueType unaryDouble(std::span<const ValueType> args) {
  requireNumeric(args);
  return valueArray(args[0]) ? ValueType::DOUBLE_ARRAY : ValueType::DOUBLE;
}

ValueType binaryNumeric(std::span<const ValueType> args) {
  requireNumeric(args);
  if (valueArray(args[0]) && valueArray(args[1])) {
    throw std::runtime_error(
        "array-to-array arithmetic is not implicit; reduce one side with min(), max(), or avg()");
  }
  bool array = valueArray(args[0]) || valueArray(args[1]);
  bool floating = valueDouble(args[0]) || valueDouble(args[1]);
  if (array) return floating ? ValueType::DOUBLE_ARRAY : ValueType::INT64_ARRAY;
  return floating ? ValueType::DOUBLE : ValueType::INT64;
}

ValueType defType(std::span<const ValueType> args) {
  requireNumeric(args);
  if (valueArray(args[0]) != valueArray(args[1])) {
    throw std::runtime_error("both arguments must be scalars or both must be arrays");
  }
  bool floating = valueDouble(args[0]) || valueDouble(args[1]);
  if (valueArray(args[0])) return floating ? ValueType::DOUBLE_ARRAY : ValueType::INT64_ARRAY;
  return floating ? ValueType::DOUBLE : ValueType::INT64;
}

ValueType minMaxType(std::span<const ValueType> args) {
  requireNumeric(args);
  if (args.size() == 1) {
    if (!valueArray(args[0])) throw std::runtime_error("the one-argument form requires an array");
    return valueScalarType(args[0]);
  }
  return binaryNumeric(args);
}

ValueType avgType(std::span<const ValueType> args) {
  requireNumeric(args);
  if (!valueArray(args[0])) throw std::runtime_error("avg() requires a numeric array");
  return ValueType::DOUBLE;
}

[[noreturn]] void runtimeInvalid(const ValueNode& node, int32_t docid,
                                 std::string_view reason) {
  throw std::runtime_error(fmt::format(
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

ValueResult evalBinaryScalar(const ValueNode& node, ValueResult left, ValueResult right,
                             int32_t docid) {
  if (!left.valid || !right.valid) return ValueResult::missing(node.type);
  bool floating = node.type == ValueType::DOUBLE;
  if (floating) {
    double a = left.type == ValueType::DOUBLE ? left.doubleValue : (double)left.intValue;
    double b = right.type == ValueType::DOUBLE ? right.doubleValue : (double)right.intValue;
    double out;
    switch (node.opcode) {
      case ValueOpcode::ADD: out = a + b; break;
      case ValueOpcode::SUB: out = a - b; break;
      case ValueOpcode::MUL: out = a * b; break;
      case ValueOpcode::DIV: out = a / b; break;
      case ValueOpcode::MIN: out = std::min(a, b); break;
      case ValueOpcode::MAX: out = std::max(a, b); break;
      default: throw std::runtime_error("invalid binary ValueExpr opcode");
    }
    if (!std::isfinite(out)) runtimeInvalid(node, docid, "produced NaN or infinity");
    return ValueResult::floating(out);
  }

  int64_t out = 0;
  bool overflow = false;
  switch (node.opcode) {
    case ValueOpcode::ADD:
      overflow = __builtin_add_overflow(left.intValue, right.intValue, &out);
      break;
    case ValueOpcode::SUB:
      overflow = __builtin_sub_overflow(left.intValue, right.intValue, &out);
      break;
    case ValueOpcode::MUL:
      overflow = __builtin_mul_overflow(left.intValue, right.intValue, &out);
      break;
    case ValueOpcode::DIV:
      if (right.intValue == 0) runtimeInvalid(node, docid, "divided by zero");
      if (left.intValue == std::numeric_limits<int64_t>::min() && right.intValue == -1) {
        runtimeInvalid(node, docid, "overflowed int64");
      }
      out = left.intValue / right.intValue;
      break;
    case ValueOpcode::MIN: out = std::min(left.intValue, right.intValue); break;
    case ValueOpcode::MAX: out = std::max(left.intValue, right.intValue); break;
    default: throw std::runtime_error("invalid binary ValueExpr opcode");
  }
  if (overflow) runtimeInvalid(node, docid, "overflowed int64");
  return ValueResult::integer(out);
}

bool unaryOpcode(ValueOpcode opcode) {
  return opcode == ValueOpcode::NEG || opcode == ValueOpcode::ABS
      || opcode == ValueOpcode::SQRT || opcode == ValueOpcode::LOG
      || opcode == ValueOpcode::LOG1P;
}

bool reducerOpcode(ValueOpcode opcode) {
  return opcode == ValueOpcode::MIN || opcode == ValueOpcode::MAX
      || opcode == ValueOpcode::AVG;
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
      ValueResult aggregate = program.evalArrayElement(first.array, 0);
      long double sum = 0.0;
      if (node.opcode == ValueOpcode::AVG) {
        sum = aggregate.type == ValueType::DOUBLE ? aggregate.doubleValue : aggregate.intValue;
      }
      for (int64_t i = 1; i < first.array.size; i++) {
        ValueResult next = program.evalArrayElement(first.array, i);
        if (node.opcode == ValueOpcode::AVG) {
          sum += next.type == ValueType::DOUBLE ? next.doubleValue : next.intValue;
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
      }
      if (node.opcode == ValueOpcode::AVG) {
        double out = (double)(sum / first.array.size);
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
    if (node.type == ValueType::INT64) {
      if (first.intValue == std::numeric_limits<int64_t>::min()) {
        runtimeInvalid(node, docid, "overflowed int64");
      }
      return ValueResult::integer(node.opcode == ValueOpcode::NEG
                                      ? -first.intValue
                                      : std::abs(first.intValue));
    }
    double input = first.type == ValueType::DOUBLE ? first.doubleValue : (double)first.intValue;
    double out;
    switch (node.opcode) {
      case ValueOpcode::NEG: out = -input; break;
      case ValueOpcode::ABS: out = std::abs(input); break;
      case ValueOpcode::SQRT: out = std::sqrt(input); break;
      case ValueOpcode::LOG: out = std::log(input); break;
      case ValueOpcode::LOG1P: out = std::log1p(input); break;
      default: std::unreachable();
    }
    if (!std::isfinite(out)) runtimeInvalid(node, docid, "produced NaN or infinity");
    return ValueResult::floating(out);
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
    results[i] = evalFunctionPoint(program, node, docids[i], score);
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
    if (scalar.type == ValueType::INT64) {
      if (first.intValue == std::numeric_limits<int64_t>::min()) {
        runtimeInvalid(node, array.docid, "overflowed int64");
      }
      return ValueResult::integer(node.opcode == ValueOpcode::NEG
                                      ? -first.intValue
                                      : std::abs(first.intValue));
    }
    double input = first.type == ValueType::DOUBLE ? first.doubleValue : (double)first.intValue;
    double out;
    switch (node.opcode) {
      case ValueOpcode::NEG: out = -input; break;
      case ValueOpcode::ABS: out = std::abs(input); break;
      case ValueOpcode::SQRT: out = std::sqrt(input); break;
      case ValueOpcode::LOG: out = std::log(input); break;
      case ValueOpcode::LOG1P: out = std::log1p(input); break;
      default: std::unreachable();
    }
    if (!std::isfinite(out)) runtimeInvalid(node, array.docid, "produced NaN or infinity");
    return ValueResult::floating(out);
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
      case ValueOpcode::DIV: {
        if (bmin <= 0 && bmax >= 0) {
          if (bmin == 0 && bmax == 0 && right.minAttained) {
            return ValueBounds::invalid(
                node.type, BoundsInvalidity::DIVIDE_BY_ZERO, mayMissing);
          }
          return ValueBounds::unbounded(node.type, mayMissing);
        }
        std::array<__int128, 4> quotients{
            (__int128)amin / bmin, (__int128)amin / bmax,
            (__int128)amax / bmin, (__int128)amax / bmax};
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
        if (bmin == 0.0 && bmax == 0.0 && right.minAttained) {
          return ValueBounds::invalid(
              node.type, BoundsInvalidity::DIVIDE_BY_ZERO, mayMissing);
        }
        return ValueBounds::unbounded(node.type, mayMissing);
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
  if (!std::isfinite(low) || !std::isfinite(high)) {
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

constexpr ValueFunction FUNCTIONS[] = {
    {ValueOpcode::DEF, "def", defType, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 2, 2},
    {ValueOpcode::ADD, "add", binaryNumeric, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 2, 2},
    {ValueOpcode::SUB, "sub", binaryNumeric, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 2, 2},
    {ValueOpcode::MUL, "mul", binaryNumeric, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 2, 2},
    {ValueOpcode::DIV, "div", binaryNumeric, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 2, 2},
    {ValueOpcode::NEG, "neg", unarySame, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 1, 1},
    {ValueOpcode::ABS, "abs", unarySame, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 1, 1},
    {ValueOpcode::SQRT, "sqrt", unaryDouble, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 1, 1},
    {ValueOpcode::LOG, "log", unaryDouble, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 1, 1},
    {ValueOpcode::LOG1P, "log1p", unaryDouble, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 1, 1},
    {ValueOpcode::MIN, "min", minMaxType, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 1, 2},
    {ValueOpcode::MAX, "max", minMaxType, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 1, 2},
    {ValueOpcode::AVG, "avg", avgType, evalFunctionPoint, evalFunctionBatch, propagateBounds,
     evalFunctionElement, 1, 1},
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
    throw std::runtime_error(fmt::format(
        "log() is invalid over the segment bounds of '{}'; use log(max({}, 1)) to clamp "
        "or log1p({}) when that is the intended transform",
        argument, argument, argument));
  }
  if (node.kind == ValueNodeKind::FUNCTION && node.opcode == ValueOpcode::LOG1P) {
    throw std::runtime_error(
        "log1p() is invalid for a value <= -1 in this segment; clamp its argument first");
  }
  if (node.kind == ValueNodeKind::FUNCTION && node.opcode == ValueOpcode::SQRT) {
    throw std::runtime_error(
        "sqrt() is invalid for a negative value in this segment; clamp its argument first");
  }
  if (bounds.invalidity == BoundsInvalidity::DIVIDE_BY_ZERO) {
    throw std::runtime_error("div() has a proven zero denominator in this segment");
  }
  if (bounds.invalidity == BoundsInvalidity::INTEGER_OVERFLOW) {
    throw std::runtime_error(fmt::format("{}() provably overflows int64 in this segment", node.text));
  }
  throw std::runtime_error(fmt::format(
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
        if (valueDouble(node.type) && bounded(bound.bounds) &&
            (!std::isfinite(bound.bounds.doubleMin) || !std::isfinite(bound.bounds.doubleMax))) {
          bound.bounds = ValueBounds::invalid(
              node.type, std::isnan(bound.bounds.doubleMin) || std::isnan(bound.bounds.doubleMax)
                             ? BoundsInvalidity::NAN_VALUE
                             : bound.bounds.doubleMin < 0.0 ? BoundsInvalidity::NEGATIVE_INFINITY
                                                            : BoundsInvalidity::POSITIVE_INFINITY,
              missing);
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
    if (!std::isfinite(decoded)) runtimeInvalid(node, docid, "read NaN or infinity");
    return ValueResult::floating(decoded);
  }
  if (node.columnType == FieldType::DOUBLE) {
    double decoded = sortableInt64ToDouble(raw);
    if (!std::isfinite(decoded)) runtimeInvalid(node, docid, "read NaN or infinity");
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
      if (!std::isfinite(decoded)) runtimeInvalid(node, array.docid, "read NaN or infinity");
      return ValueResult::floating(decoded);
    }
    if (node.columnType == FieldType::DOUBLE) {
      double decoded = sortableInt64ToDouble(raw);
      if (!std::isfinite(decoded)) runtimeInvalid(node, array.docid, "read NaN or infinity");
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

} // namespace solux
