// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Math and bitwise functions of the value language: trigonometry, rounding,
// logarithms, modulo, bit operators and integer conversion. Operators map onto
// these by name (% is mod, & is bit_and, and so on), see ValueExprGrammar.h.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "luxir/value/ValueFunctionSupport.h"

namespace luxir {
namespace {

using namespace valuefn;

constexpr double INF = std::numeric_limits<double>::infinity();

// ---------------------------------------------------------------------------
// One double argument, one double result. A NaN or infinite result is an
// evaluation error, as for log() and sqrt(). Array arguments map element-wise.
// Low and High, when finite, are the proven range of every result.
enum class Domain { ALL, NONNEGATIVE, POSITIVE, ABOVE_MINUS_ONE, UNIT, AT_LEAST_ONE, OPEN_UNIT };

enum class Curve { INCREASING, DECREASING, COSH, PERIODIC };

// Bounds use the same operation as point, batch, element and bucket kernels.
// An endpoint outside the domain proves an error only when it is attained.
template <auto Fn, Curve Shape = Curve::INCREASING, double Low = -INF, double High = INF,
          Domain InputDomain = Domain::ALL>
struct Math1 {
  static ValueResult apply(const ValueNode& node, const ValueResult& input, int32_t docid) {
    if (!input.valid) return ValueResult::missing(ValueType::DOUBLE);
    double out = Fn(numberOf(input));
    if (!std::isfinite(out)) runtimeInvalid(node, docid, "produced NaN or infinity");
    return ValueResult::floating(out);
  }

  static ValueResult point(BoundValueProgram& program, const ValueNode& node,
                           int32_t docid, float score) {
    ValueResult input = program.evalNode(node.children[0], docid, score);
    if (!input.valid) return ValueResult::missing(node.type);
    if (valueArray(input.type)) {
      return ValueResult::arrayValue(node.type, nodeIndex(program, node), docid, score, input.array.size);
    }
    return apply(node, input, docid);
  }

  static ValueResult element(BoundValueProgram& program, const ValueNode& node,
                             const ValueArrayRef& array, int64_t index) {
    ValueResult value = program.evalNode(node.children[0], array.docid, array.score);
    return apply(node, valueArray(value.type) ? program.evalArrayElement(value.array, index) : value, array.docid);
  }

  static ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds> args) {
    const ValueBounds& input = args[0];
    if (input.alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
    if (!bounded(input)) {
      if constexpr (InputDomain == Domain::ALL && std::isfinite(Low) && std::isfinite(High)) {
        return boundedResult(node, Low, High, input.mayBeMissing);
      }
      return ValueBounds::unbounded(node.type, input.mayBeMissing);
    }
    double low = boundMinAsDouble(input), high = boundMaxAsDouble(input);
    auto inside = [](double x) {
      switch (InputDomain) {
        case Domain::ALL: return true;
        case Domain::NONNEGATIVE: return x >= 0;
        case Domain::POSITIVE: return x > 0;
        case Domain::ABOVE_MINUS_ONE: return x > -1;
        case Domain::UNIT: return x >= -1 && x <= 1;
        case Domain::AT_LEAST_ONE: return x >= 1;
        case Domain::OPEN_UNIT: return x > -1 && x < 1;
      }
      std::unreachable();
    };
    bool lowDomain = !inside(low), highDomain = !inside(high);
    double a = lowDomain ? std::numeric_limits<double>::quiet_NaN() : Fn(low);
    double b = highDomain ? std::numeric_limits<double>::quiet_NaN() : Fn(high);
    bool lowFailed = !std::isfinite(a), highFailed = !std::isfinite(b);
    if (lowFailed || highFailed) {
      if ((lowFailed && input.minAttained) || (highFailed && input.maxAttained)) {
        double failed = lowFailed && input.minAttained ? a : b;
        return constantDouble(node, failed, input.mayBeMissing);
      }
      return ValueBounds::unbounded(node.type, input.mayBeMissing);
    }
    bool minAttained = input.minAttained, maxAttained = input.maxAttained;
    if constexpr (Shape == Curve::PERIODIC) {
      if (!singleton(input)) {
        if constexpr (std::isfinite(Low) && std::isfinite(High)) {
          return boundedResult(node, Low, High, input.mayBeMissing);
        }
        return ValueBounds::unbounded(node.type, input.mayBeMissing);
      }
    } else if constexpr (Shape == Curve::DECREASING) {
      std::swap(a, b);
      std::swap(minAttained, maxAttained);
    } else if constexpr (Shape == Curve::COSH) {
      minAttained = (a <= b && input.minAttained) || (b <= a && input.maxAttained);
      maxAttained = (a >= b && input.minAttained) || (b >= a && input.maxAttained);
      double lower = std::min(a, b), upper = std::max(a, b);
      if (low <= 0 && high >= 0) {
        lower = 1;
        minAttained = (low == 0 && input.minAttained) || (high == 0 && input.maxAttained);
      }
      a = lower; b = upper;
    }
    ValueBounds out = boundedResult(node, a, b, input.mayBeMissing);
    out.minAttained = minAttained;
    out.maxAttained = maxAttained;
    return out;
  }

  static BucketScalar bucket(std::span<const BucketScalar> args, ValueType, ValueNature) {
    if (!args[0].valid) return BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER);
    return bucketDouble(Fn(bucketNumber(args[0])));
  }
};

template <auto Fn, Curve Shape = Curve::INCREASING, double Low = -INF, double High = INF,
          Domain InputDomain = Domain::ALL>
constexpr ValueFunction unary(std::string_view name, ValueOpcode opcode = ValueOpcode::NONE) {
  using M = Math1<Fn, Shape, Low, High, InputDomain>;
  return {opcode, name, unaryDouble, M::point, evalFunctionBatch, M::bounds, M::element, 1, 1, {RAW}, M::bucket};
}

ResolvedValue binaryDoubleResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  return {ValueType::DOUBLE, ValueNature::NUMBER};
}

ResolvedValue binaryArrayDoubleResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  if (valueArray(args[0].type) && valueArray(args[1].type)) {
    throw std::runtime_error("array-to-array arithmetic is not implicit; reduce one side first");
  }
  return {valueArray(args[0].type) || valueArray(args[1].type) ? ValueType::DOUBLE_ARRAY : ValueType::DOUBLE,
          ValueNature::NUMBER};
}

template <auto Fn, bool MapsArrays = false>
struct Math2 {
  static ValueResult apply(const ValueNode& node, ValueResult a, ValueResult b, int32_t docid) {
    if (!a.valid || !b.valid) return ValueResult::missing(ValueType::DOUBLE);
    double out = Fn(numberOf(a), numberOf(b));
    if (!std::isfinite(out)) runtimeInvalid(node, docid, "produced NaN or infinity");
    return ValueResult::floating(out);
  }

  static ValueResult point(BoundValueProgram& program, const ValueNode& node, int32_t docid, float score) {
    ValueResult a = program.evalNode(node.children[0], docid, score);
    ValueResult b = program.evalNode(node.children[1], docid, score);
    if constexpr (MapsArrays) {
      if (!a.valid || !b.valid) return ValueResult::missing(node.type);
      if (valueArray(node.type)) {
        int64_t size = valueArray(a.type) ? a.array.size : b.array.size;
        return ValueResult::arrayValue(node.type, nodeIndex(program, node), docid, score, size);
      }
    }
    return apply(node, a, b, docid);
  }

  static ValueResult element(BoundValueProgram& program, const ValueNode& node,
                             const ValueArrayRef& array, int64_t index) {
    auto item = [&](uint32_t child) {
      ValueResult value = program.evalNode(child, array.docid, array.score);
      return valueArray(value.type) ? program.evalArrayElement(value.array, index) : value;
    };
    return apply(node, item(node.children[0]), item(node.children[1]), array.docid);
  }

  static ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds> args) {
    if (args[0].alwaysMissing || args[1].alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
    bool missing = args[0].mayBeMissing || args[1].mayBeMissing;
    if (singleton(args[0]) && singleton(args[1])) {
      double out = Fn(boundMinAsDouble(args[0]), boundMinAsDouble(args[1]));
      bool attained = args[0].minAttained && args[1].minAttained
          && (!args[0].mayBeMissing || !args[1].mayBeMissing);
      if (std::isfinite(out) || attained) return constantDouble(node, out, missing);
    }
    return ValueBounds::unbounded(node.type, missing);
  }

  static BucketScalar bucket(std::span<const BucketScalar> args, ValueType, ValueNature) {
    if (!args[0].valid || !args[1].valid) return BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER);
    return bucketDouble(Fn(bucketNumber(args[0]), bucketNumber(args[1])));
  }
};

template <auto Fn, bool MapsArrays = false>
constexpr ValueFunction binary(std::string_view name, ValueOpcode opcode = ValueOpcode::NONE,
                              ValueFunction::BoundsPropagate bounds = Math2<Fn, MapsArrays>::bounds) {
  using M = Math2<Fn, MapsArrays>;
  return {opcode, name, MapsArrays ? binaryArrayDoubleResolve : binaryDoubleResolve, M::point,
          evalFunctionBatch, bounds, MapsArrays ? M::element : evalScalarOnlyElement,
          2, 2, {RAW, RAW}, M::bucket};
}

// ---------------------------------------------------------------------------
// Constants: pi() and e().
template <double Value>
struct Constant {
  static ResolvedValue resolve(std::span<const ResolvedValue>) {
    return {ValueType::DOUBLE, ValueNature::NUMBER};
  }
  static ValueResult point(BoundValueProgram&, const ValueNode&, int32_t, float) {
    return ValueResult::floating(Value);
  }
  static ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds>) {
    return constantDouble(node, Value, false);
  }
  static BucketScalar bucket(std::span<const BucketScalar>, ValueType, ValueNature) {
    return BucketScalar::floating(Value);
  }
};

template <double Value>
constexpr ValueFunction constant(std::string_view name) {
  using M = Constant<Value>;
  return {ValueOpcode::NONE, name, M::resolve, M::point, evalFunctionBatch, M::bounds,
          evalScalarOnlyElement, 0, 0, {}, M::bucket};
}

// ---------------------------------------------------------------------------
// Integer operators. Operands are int64 scalars; a missing operand is missing.
// Shift distances use their low six bits, as in Java and Painless.
ResolvedValue integerResolve(std::span<const ResolvedValue> args) {
  requireIntegers(args);
  return {ValueType::INT64, ValueNature::NUMBER};
}

template <auto Fn>
struct IntBinary {
  static ValueResult point(BoundValueProgram& program, const ValueNode& node,
                           int32_t docid, float score) {
    ValueResult a = program.evalNode(node.children[0], docid, score);
    ValueResult b = program.evalNode(node.children[1], docid, score);
    if (!a.valid || !b.valid) return ValueResult::missing(node.type);
    return ValueResult::integer(Fn(a.intValue, b.intValue));
  }

  static ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds> args) {
    if (args[0].alwaysMissing || args[1].alwaysMissing) {
      return ValueBounds::unbounded(node.type, true, true);
    }
    bool mayBeMissing = args[0].mayBeMissing || args[1].mayBeMissing;
    if (singleton(args[0]) && singleton(args[1])) {
      return constantInteger(node, Fn(args[0].intMin, args[1].intMin), mayBeMissing);
    }
    return ValueBounds::unbounded(node.type, mayBeMissing);
  }

  static BucketScalar bucket(std::span<const BucketScalar> args, ValueType, ValueNature) {
    if (!args[0].valid || !args[1].valid) {
      return BucketScalar::missing(BucketValueType::INT128, ValueNature::NUMBER);
    }
    for (const BucketScalar& arg : args) {
      if (arg.intValue < INT64_MIN || arg.intValue > INT64_MAX) {
        return BucketScalar::failed(BucketValueType::INT128, ValueNature::NUMBER,
                                    AggregateFailure::INT128_OVERFLOW);
      }
    }
    return BucketScalar::integer(Fn((int64_t)args[0].intValue, (int64_t)args[1].intValue));
  }
};

template <auto Fn>
constexpr ValueFunction integerBinary(std::string_view name) {
  using M = IntBinary<Fn>;
  return {ValueOpcode::NONE, name, integerResolve, M::point, evalFunctionBatch, M::bounds,
          evalScalarOnlyElement, 2, 2, {RAW, RAW}, M::bucket};
}

inline constexpr auto BIT_AND = [](int64_t a, int64_t b) { return a & b; };
inline constexpr auto BIT_OR = [](int64_t a, int64_t b) { return a | b; };
inline constexpr auto BIT_XOR = [](int64_t a, int64_t b) { return a ^ b; };
inline constexpr auto SHL = [](int64_t a, int64_t b) {
  return (int64_t)((uint64_t)a << (b & 63));
};
inline constexpr auto SHR = [](int64_t a, int64_t b) { return a >> (b & 63); };
inline constexpr auto USHR = [](int64_t a, int64_t b) {
  return (int64_t)((uint64_t)a >> (b & 63));
};

ValueResult bitNotPoint(BoundValueProgram& program, const ValueNode& node,
                        int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid) return ValueResult::missing(node.type);
  return ValueResult::integer(~input.intValue);
}

ValueBounds bitNotBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  if (args[0].alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
  if (singleton(args[0])) return constantInteger(node, ~args[0].intMin, args[0].mayBeMissing);
  if (bounded(args[0])) {
    // ~x reverses the order, so the interval maps to [~max, ~min] without overflow.
    ValueBounds out = ValueBounds::integerRange(~args[0].intMax, ~args[0].intMin, args[0].mayBeMissing);
    out.type = node.type;
    out.minAttained = args[0].maxAttained;
    out.maxAttained = args[0].minAttained;
    return out;
  }
  return ValueBounds::unbounded(node.type, args[0].mayBeMissing);
}

BucketScalar bitNotBucket(std::span<const BucketScalar> args, ValueType, ValueNature) {
  if (!args[0].valid) return BucketScalar::missing(BucketValueType::INT128, ValueNature::NUMBER);
  if (args[0].intValue < INT64_MIN || args[0].intValue > INT64_MAX) {
    return BucketScalar::failed(BucketValueType::INT128, ValueNature::NUMBER,
                                AggregateFailure::INT128_OVERFLOW);
  }
  return BucketScalar::integer(~(int64_t)args[0].intValue);
}

// ---------------------------------------------------------------------------
// mod(a, b), the % operator: Java's remainder (the sign follows the dividend),
// on integers or doubles. A zero divisor makes the value missing, as for /.
ResolvedValue modResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  bool floating = valueDouble(args[0].type) || valueDouble(args[1].type);
  return {floating ? ValueType::DOUBLE : ValueType::INT64, ValueNature::NUMBER};
}

ValueResult modPoint(BoundValueProgram& program, const ValueNode& node,
                     int32_t docid, float score) {
  ValueResult a = program.evalNode(node.children[0], docid, score);
  ValueResult b = program.evalNode(node.children[1], docid, score);
  if (!a.valid || !b.valid) return ValueResult::missing(node.type);
  if (node.type == ValueType::INT64) {
    if (b.intValue == 0) return ValueResult::missing(node.type);
    return ValueResult::integer(b.intValue == -1 ? 0 : a.intValue % b.intValue);
  }
  double divisor = numberOf(b);
  if (divisor == 0.0) return ValueResult::missing(node.type);
  double out = std::fmod(numberOf(a), divisor);
  if (!std::isfinite(out)) runtimeInvalid(node, docid, "produced NaN or infinity");
  return ValueResult::floating(out);
}

ValueBounds modBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  if (args[0].alwaysMissing || args[1].alwaysMissing) {
    return ValueBounds::unbounded(node.type, true, true);
  }
  bool mayBeMissing = args[0].mayBeMissing || args[1].mayBeMissing;
  if (singleton(args[0]) && singleton(args[1])) {
    if (node.type == ValueType::INT64) {
      int64_t a = args[0].intMin;
      int64_t b = args[1].intMin;
      if (b == 0) return ValueBounds::unbounded(node.type, true, true);
      return constantInteger(node, b == -1 ? 0 : a % b, mayBeMissing);
    }
    double b = boundMinAsDouble(args[1]);
    if (b == 0.0) return ValueBounds::unbounded(node.type, true, true);
    double out = std::fmod(boundMinAsDouble(args[0]), b);
    if (std::isfinite(out)) return constantDouble(node, out, mayBeMissing);
  }
  // A divisor that may be zero can make any value missing.
  return ValueBounds::unbounded(node.type, true);
}

BucketScalar modBucket(std::span<const BucketScalar> args, ValueType type, ValueNature) {
  BucketValueType bucketType = type == ValueType::DOUBLE
      ? BucketValueType::DOUBLE : BucketValueType::INT128;
  if (!args[0].valid || !args[1].valid) {
    return BucketScalar::missing(bucketType, ValueNature::NUMBER);
  }
  if (type == ValueType::DOUBLE) {
    double divisor = bucketNumber(args[1]);
    if (divisor == 0.0) return BucketScalar::missing(bucketType, ValueNature::NUMBER);
    return bucketDouble(std::fmod(bucketNumber(args[0]), divisor));
  }
  if (args[1].intValue == 0) return BucketScalar::missing(bucketType, ValueNature::NUMBER);
  return BucketScalar::integer(args[1].intValue == -1 ? 0 : args[0].intValue % args[1].intValue);
}

// ---------------------------------------------------------------------------
// to_int(x) truncates toward zero to an int64 and fails when the value does
// not fit; to_double(x) widens an integer.
ResolvedValue toIntResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  return {ValueType::INT64, ValueNature::NUMBER};
}

ResolvedValue toDoubleResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  return {ValueType::DOUBLE, ValueNature::NUMBER};
}

bool fitsInt64(double value) {
  return value >= -0x1p63 && value < 0x1p63;
}

ValueResult toIntPoint(BoundValueProgram& program, const ValueNode& node,
                       int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid) return ValueResult::missing(node.type);
  if (input.type == ValueType::INT64) return input;
  if (!fitsInt64(input.doubleValue)) runtimeInvalid(node, docid, "overflowed int64");
  return ValueResult::integer((int64_t)input.doubleValue);
}

ValueBounds toIntBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  const ValueBounds& input = args[0];
  if (input.alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
  if (!bounded(input)) return ValueBounds::unbounded(node.type, input.mayBeMissing);
  if (input.type == ValueType::INT64) return input;
  if (!fitsInt64(input.doubleMin) || !fitsInt64(input.doubleMax)) {
    bool attained = (!fitsInt64(input.doubleMin) && input.minAttained)
        || (!fitsInt64(input.doubleMax) && input.maxAttained);
    return attained ? ValueBounds::invalid(node.type, BoundsInvalidity::INTEGER_OVERFLOW, input.mayBeMissing)
                    : ValueBounds::unbounded(node.type, input.mayBeMissing);
  }
  auto out = ValueBounds::integerRange((int64_t)input.doubleMin, (int64_t)input.doubleMax, input.mayBeMissing);
  out.minAttained = input.minAttained;
  out.maxAttained = input.maxAttained;
  return out;
}

BucketScalar toIntBucket(std::span<const BucketScalar> args, ValueType, ValueNature) {
  if (!args[0].valid) return BucketScalar::missing(BucketValueType::INT128, ValueNature::NUMBER);
  if (args[0].type == BucketValueType::INT128) return BucketScalar::integer(args[0].intValue);
  if (!fitsInt64(args[0].doubleValue)) {
    return BucketScalar::failed(BucketValueType::INT128, ValueNature::NUMBER,
                                AggregateFailure::INT128_OVERFLOW);
  }
  return BucketScalar::integer((int64_t)args[0].doubleValue);
}

ValueResult toDoublePoint(BoundValueProgram& program, const ValueNode& node,
                          int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid) return ValueResult::missing(node.type);
  return ValueResult::floating(numberOf(input));
}

ValueBounds toDoubleBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  const ValueBounds& input = args[0];
  if (input.alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
  if (!bounded(input)) return ValueBounds::unbounded(node.type, input.mayBeMissing);
  ValueBounds out = ValueBounds::floatingRange(boundMinAsDouble(input), boundMaxAsDouble(input),
                                          input.mayBeMissing);
  out.type = node.type;
  out.minAttained = input.minAttained;
  out.maxAttained = input.maxAttained;
  return out;
}

BucketScalar toDoubleBucket(std::span<const BucketScalar> args, ValueType, ValueNature) {
  if (!args[0].valid) return BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER);
  return BucketScalar::floating(bucketNumber(args[0]));
}

// ---------------------------------------------------------------------------
// clamp(x, low, high) is min(max(x, low), high). Integers stay integers.
ResolvedValue clampResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  bool floating = std::ranges::any_of(args, [](const ResolvedValue& arg) {
    return valueDouble(arg.type);
  });
  return {floating ? ValueType::DOUBLE : ValueType::INT64, ValueNature::NUMBER};
}

ValueResult clampPoint(BoundValueProgram& program, const ValueNode& node,
                       int32_t docid, float score) {
  ValueResult x = program.evalNode(node.children[0], docid, score);
  ValueResult low = program.evalNode(node.children[1], docid, score);
  ValueResult high = program.evalNode(node.children[2], docid, score);
  if (!x.valid || !low.valid || !high.valid) return ValueResult::missing(node.type);
  if (node.type == ValueType::INT64) {
    return ValueResult::integer(std::min(std::max(x.intValue, low.intValue), high.intValue));
  }
  return ValueResult::floating(std::min(std::max(numberOf(x), numberOf(low)), numberOf(high)));
}

ValueBounds clampBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  bool mayBeMissing = args[0].mayBeMissing || args[1].mayBeMissing || args[2].mayBeMissing;
  if (args[0].alwaysMissing || args[1].alwaysMissing || args[2].alwaysMissing) {
    return ValueBounds::unbounded(node.type, true, true);
  }
  if (!singleton(args[1]) || !singleton(args[2])) {
    return ValueBounds::unbounded(node.type, mayBeMissing);
  }
  if (node.type == ValueType::INT64) {
    int64_t low = args[1].intMin;
    int64_t high = args[2].intMin;
    auto clamped = [&](int64_t value) { return std::min(std::max(value, low), high); };
    int64_t min = bounded(args[0]) ? clamped(args[0].intMin) : clamped(INT64_MIN);
    int64_t max = bounded(args[0]) ? clamped(args[0].intMax) : clamped(INT64_MAX);
    ValueBounds out = ValueBounds::integerRange(min, max, mayBeMissing);
    out.type = node.type;
    out.minAttained = out.maxAttained = !mayBeMissing && bounded(args[0]) && singleton(args[0]);
    return out;
  }
  double low = boundMinAsDouble(args[1]);
  double high = boundMinAsDouble(args[2]);
  auto clamped = [&](double value) { return std::min(std::max(value, low), high); };
  double min = bounded(args[0]) ? clamped(boundMinAsDouble(args[0])) : clamped(-INF);
  double max = bounded(args[0]) ? clamped(boundMaxAsDouble(args[0])) : clamped(INF);
  ValueBounds out = ValueBounds::floatingRange(min, max, mayBeMissing);
  out.type = node.type;
  out.minAttained = out.maxAttained = !mayBeMissing && bounded(args[0]) && singleton(args[0]);
  return out;
}

BucketScalar clampBucket(std::span<const BucketScalar> args, ValueType type, ValueNature) {
  BucketValueType bucketType = type == ValueType::DOUBLE
      ? BucketValueType::DOUBLE : BucketValueType::INT128;
  if (!args[0].valid || !args[1].valid || !args[2].valid) {
    return BucketScalar::missing(bucketType, ValueNature::NUMBER);
  }
  if (type == ValueType::DOUBLE) {
    return BucketScalar::floating(std::min(std::max(bucketNumber(args[0]), bucketNumber(args[1])),
                                           bucketNumber(args[2])));
  }
  return BucketScalar::integer(std::min(std::max(args[0].intValue, args[1].intValue), args[2].intValue));
}

// ---------------------------------------------------------------------------
// The scalar functions. Java semantics where Java defines one: round() rounds
// half up (floor(x + 0.5) without the precision loss), and to_radians and
// to_degrees multiply by the same constants as Math.toRadians and
// Math.toDegrees.
inline constexpr auto SIN = [](double x) { return std::sin(x); };
inline constexpr auto COS = [](double x) { return std::cos(x); };
inline constexpr auto TAN = [](double x) { return std::tan(x); };
inline constexpr auto ASIN = [](double x) { return std::asin(x); };
inline constexpr auto ACOS = [](double x) { return std::acos(x); };
inline constexpr auto ATAN = [](double x) { return std::atan(x); };
inline constexpr auto SINH = [](double x) { return std::sinh(x); };
inline constexpr auto COSH = [](double x) { return std::cosh(x); };
inline constexpr auto TANH = [](double x) { return std::tanh(x); };
inline constexpr auto ASINH = [](double x) { return std::asinh(x); };
inline constexpr auto ACOSH = [](double x) { return std::acosh(x); };
inline constexpr auto ATANH = [](double x) { return std::atanh(x); };
inline constexpr auto SQRT = [](double x) { return std::sqrt(x); };
inline constexpr auto LOG1P = [](double x) { return std::log1p(x); };
inline constexpr auto FLOOR = [](double x) { return std::floor(x); };
inline constexpr auto EXP = [](double x) { return std::exp(x); };
inline constexpr auto POW = [](double a, double b) { return std::pow(a, b); };
inline constexpr auto LOG = [](double x) { return std::log(x); };
inline constexpr auto LOG2 = [](double x) { return std::log2(x); };
inline constexpr auto LOG10 = [](double x) { return std::log10(x); };
inline constexpr auto EXPM1 = [](double x) { return std::expm1(x); };
inline constexpr auto CBRT = [](double x) { return std::cbrt(x); };
inline constexpr auto CEIL = [](double x) { return std::ceil(x); };
inline constexpr auto TRUNC = [](double x) { return std::trunc(x); };
inline constexpr auto RINT = [](double x) { return std::nearbyint(x); };
inline constexpr auto ROUND = [](double x) {
  double floor = std::floor(x);
  return x - floor >= 0.5 ? floor + 1.0 : floor;
};
inline constexpr auto SIGNUM = [](double x) {
  return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : x;
};
inline constexpr auto TO_RADIANS = [](double x) { return x * 0.017453292519943295; };
inline constexpr auto TO_DEGREES = [](double x) { return x * 57.29577951308232; };
inline constexpr auto ATAN2 = [](double y, double x) { return std::atan2(y, x); };
inline constexpr auto HYPOT = [](double a, double b) { return std::hypot(a, b); };

constexpr double HALF_PI = std::numbers::pi / 2.0;

ValueBounds powBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  const ValueBounds& base = args[0];
  const ValueBounds& exponent = args[1];
  if (invalid(base)) return retag(base, node.type);
  if (invalid(exponent)) return retag(exponent, node.type);
  bool mayMissing = base.mayBeMissing || exponent.mayBeMissing;
  if (base.alwaysMissing || exponent.alwaysMissing) {
    return ValueBounds::unbounded(node.type, true, true);
  }
  if (!bounded(base) || !bounded(exponent)) {
    return ValueBounds::unbounded(node.type, mayMissing);
  }
  double amin = boundMinAsDouble(base);
  double amax = boundMaxAsDouble(base);
  double e = boundMinAsDouble(exponent);
  if (e != boundMaxAsDouble(exponent)) return ValueBounds::unbounded(node.type, mayMissing);
  double low;
  double high;
  bool exact = amin == amax;
  if (exact) {
    low = high = std::pow(amin, e);
  } else if (e == 0.0) {
    low = high = 1.0;
  } else if (amin >= 0.0 && e > 0.0) {
    low = std::pow(amin, e);
    high = std::pow(amax, e);
  } else if (amin > 0.0 && e < 0.0) {
    low = std::pow(amax, e);
    high = std::pow(amin, e);
  } else {
    return ValueBounds::unbounded(node.type, mayMissing);
  }
  if (!std::isfinite(low) || !std::isfinite(high)) {
    if (exact && base.minAttained && exponent.minAttained && (!base.mayBeMissing || !exponent.mayBeMissing)) {
      return ValueBounds::invalid(
          node.type, std::isnan(low) ? BoundsInvalidity::NAN_VALUE
                                     : BoundsInvalidity::POSITIVE_INFINITY,
          mayMissing);
    }
    return ValueBounds::unbounded(node.type, mayMissing);
  }
  ValueBounds out = ValueBounds::floatingRange(low, high, mayMissing);
  out.type = node.type;
  out.minAttained = exact && base.minAttained && exponent.minAttained && (!base.mayBeMissing || !exponent.mayBeMissing);
  out.maxAttained = out.minAttained;
  return out;
}

inline constexpr auto LOGN = [](double base, double value) {
  if (!(base > 0 && base != 1 && value > 0)) return std::numeric_limits<double>::quiet_NaN();
  return std::log(value) / std::log(base);
};

ValueBounds lognBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  const auto& base = args[0];
  const auto& value = args[1];
  if (base.alwaysMissing || value.alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
  bool missing = base.mayBeMissing || value.mayBeMissing;
  if (!singleton(base) || !bounded(value)) return Math2<LOGN>::bounds(node, args);
  double b = boundMinAsDouble(base);
  double low = boundMinAsDouble(value), high = boundMaxAsDouble(value);
  bool baseObserved = base.minAttained || base.maxAttained;
  bool valueObserved = value.minAttained || value.maxAttained;
  if (!(b > 0 && b != 1)) {
    bool paired = (baseObserved && !value.mayBeMissing)
        || (valueObserved && !base.mayBeMissing);
    return paired ? ValueBounds::invalid(node.type, BoundsInvalidity::DOMAIN_ERROR, missing)
                  : ValueBounds::unbounded(node.type, missing);
  }
  auto pairedEndpoint = [&](bool attained) {
    return (attained && !base.mayBeMissing)
        || (singleton(value) && !value.mayBeMissing && baseObserved);
  };
  double a = LOGN(b, low), z = LOGN(b, high);
  if (!std::isfinite(a) || !std::isfinite(z)) {
    if ((!std::isfinite(a) && pairedEndpoint(value.minAttained))
        || (!std::isfinite(z) && pairedEndpoint(value.maxAttained))) {
      return constantDouble(node, !std::isfinite(a) && pairedEndpoint(value.minAttained) ? a : z, missing);
    }
    return ValueBounds::unbounded(node.type, missing);
  }
  ValueBounds out = boundedResult(node, a, z, missing);
  out.minAttained = pairedEndpoint(value.minAttained);
  out.maxAttained = pairedEndpoint(value.maxAttained);
  if (b < 1) {
    std::swap(out.doubleMin, out.doubleMax);
    std::swap(out.minAttained, out.maxAttained);
  }
  return out;
}

constexpr ValueFunction FUNCTIONS[] = {
    unary<SIN, Curve::PERIODIC, -1.0, 1.0>("sin"),
    unary<COS, Curve::PERIODIC, -1.0, 1.0>("cos"),
    unary<TAN, Curve::PERIODIC>("tan"),
    unary<ASIN, Curve::INCREASING, -INF, INF, Domain::UNIT>("asin"),
    unary<ACOS, Curve::DECREASING, -INF, INF, Domain::UNIT>("acos"),
    unary<ATAN, Curve::INCREASING, -HALF_PI, HALF_PI>("atan"),
    unary<SINH>("sinh"),
    unary<COSH, Curve::COSH, 1.0, INF>("cosh"),
    unary<TANH, Curve::INCREASING, -1.0, 1.0>("tanh"),
    unary<ASINH>("asinh"),
    unary<ACOSH, Curve::INCREASING, -INF, INF, Domain::AT_LEAST_ONE>("acosh"),
    unary<ATANH, Curve::INCREASING, -INF, INF, Domain::OPEN_UNIT>("atanh"),
    unary<LOG, Curve::INCREASING, -INF, INF, Domain::POSITIVE>("log", ValueOpcode::LOG),
    unary<LOG1P, Curve::INCREASING, -INF, INF, Domain::ABOVE_MINUS_ONE>("log1p", ValueOpcode::LOG1P),
    unary<SQRT, Curve::INCREASING, -INF, INF, Domain::NONNEGATIVE>("sqrt", ValueOpcode::SQRT),
    unary<FLOOR>("floor", ValueOpcode::FLOOR),
    unary<EXP>("exp", ValueOpcode::EXP),
    binary<POW, true>("pow", ValueOpcode::POW, powBounds),
    unary<LOG2, Curve::INCREASING, -INF, INF, Domain::POSITIVE>("log2"),
    unary<LOG10, Curve::INCREASING, -INF, INF, Domain::POSITIVE>("log10"),
    unary<EXPM1, Curve::INCREASING, -1.0, INF>("expm1"),
    unary<CBRT>("cbrt"),
    unary<CEIL>("ceil"),
    unary<ROUND>("round"),
    unary<RINT>("rint"),
    unary<TRUNC>("trunc"),
    unary<SIGNUM, Curve::INCREASING, -1.0, 1.0>("signum"),
    unary<TO_RADIANS>("to_radians"),
    unary<TO_DEGREES>("to_degrees"),
    binary<ATAN2>("atan2"),
    binary<HYPOT>("hypot"),
    binary<LOGN>("logn", ValueOpcode::NONE, lognBounds),
    constant<std::numbers::pi>("pi"),
    constant<std::numbers::e>("e"),
    integerBinary<BIT_AND>("bit_and"),
    integerBinary<BIT_OR>("bit_or"),
    integerBinary<BIT_XOR>("bit_xor"),
    integerBinary<SHL>("shl"),
    integerBinary<SHR>("shr"),
    integerBinary<USHR>("ushr"),
    {ValueOpcode::NONE, "bit_not", integerResolve, bitNotPoint, evalFunctionBatch, bitNotBounds,
     evalScalarOnlyElement, 1, 1, {RAW}, bitNotBucket},
    {ValueOpcode::NONE, "mod", modResolve, modPoint, evalFunctionBatch, modBounds,
     evalScalarOnlyElement, 2, 2, {RAW, RAW}, modBucket},
    {ValueOpcode::NONE, "to_int", toIntResolve, toIntPoint, evalFunctionBatch, toIntBounds,
     evalScalarOnlyElement, 1, 1, {RAW}, toIntBucket},
    {ValueOpcode::NONE, "to_double", toDoubleResolve, toDoublePoint, evalFunctionBatch,
     toDoubleBounds, evalScalarOnlyElement, 1, 1, {RAW}, toDoubleBucket},
    {ValueOpcode::NONE, "clamp", clampResolve, clampPoint, evalFunctionBatch, clampBounds,
     evalScalarOnlyElement, 3, 3, {RAW, RAW, RAW}, clampBucket},
};

static_assert(badOperandFlags(FUNCTIONS).empty(), badOperandFlags(FUNCTIONS));

} // namespace

std::span<const ValueFunction> valueMathFunctions() {
  return FUNCTIONS;
}

} // namespace luxir
