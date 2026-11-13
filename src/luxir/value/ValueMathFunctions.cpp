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

constexpr ValueFunction::Operand RAW{ValueFunction::DateUnit::RAW};
constexpr double INF = std::numeric_limits<double>::infinity();

// ---------------------------------------------------------------------------
// One double argument, one double result. A NaN or infinite result is an
// evaluation error, as for log() and sqrt(). Array arguments map element-wise.
// Low and High, when finite, are the proven range of every result.
template <auto Fn, double Low = -INF, double High = INF>
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
    const ValueBounds& input = args[0];
    if (input.alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
    if (singleton(input)) {
      double out = Fn(boundMinAsDouble(input));
      if (std::isfinite(out)) return constantDouble(node, out, input.mayBeMissing);
      if (input.minAttained && input.maxAttained) {
        return constantDouble(node, out, input.mayBeMissing);
      }
    }
    if constexpr (std::isfinite(Low) && std::isfinite(High)) {
      return boundedResult(node, Low, High, input.mayBeMissing);
    }
    return ValueBounds::unbounded(node.type, input.mayBeMissing);
  }

  static BucketScalar bucket(std::span<const BucketScalar> args, ValueType, ValueNature) {
    if (!args[0].valid) return BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER);
    return bucketDouble(Fn(bucketNumber(args[0])));
  }
};

template <auto Fn, double Low = -INF, double High = INF>
constexpr ValueFunction unary(std::string_view name) {
  using M = Math1<Fn, Low, High>;
  return {ValueOpcode::NONE, name, unaryDouble, M::point, evalFunctionBatch, M::bounds,
          M::element, 1, 1, {RAW}, M::bucket};
}

// ---------------------------------------------------------------------------
// Two scalar arguments, a double result.
ResolvedValue binaryDoubleResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  return {ValueType::DOUBLE, ValueNature::NUMBER};
}

template <auto Fn>
struct Math2 {
  static ValueResult point(BoundValueProgram& program, const ValueNode& node,
                           int32_t docid, float score) {
    ValueResult a = program.evalNode(node.children[0], docid, score);
    ValueResult b = program.evalNode(node.children[1], docid, score);
    if (!a.valid || !b.valid) return ValueResult::missing(node.type);
    double out = Fn(numberOf(a), numberOf(b));
    if (!std::isfinite(out)) runtimeInvalid(node, docid, "produced NaN or infinity");
    return ValueResult::floating(out);
  }

  static ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds> args) {
    if (args[0].alwaysMissing || args[1].alwaysMissing) {
      return ValueBounds::unbounded(node.type, true, true);
    }
    bool mayBeMissing = args[0].mayBeMissing || args[1].mayBeMissing;
    if (singleton(args[0]) && singleton(args[1])) {
      double out = Fn(boundMinAsDouble(args[0]), boundMinAsDouble(args[1]));
      bool attained = args[0].minAttained && args[0].maxAttained
          && args[1].minAttained && args[1].maxAttained;
      if (std::isfinite(out) || attained) return constantDouble(node, out, mayBeMissing);
    }
    return ValueBounds::unbounded(node.type, mayBeMissing);
  }

  static BucketScalar bucket(std::span<const BucketScalar> args, ValueType, ValueNature) {
    if (!args[0].valid || !args[1].valid) {
      return BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER);
    }
    return bucketDouble(Fn(bucketNumber(args[0]), bucketNumber(args[1])));
  }
};

template <auto Fn>
constexpr ValueFunction binary(std::string_view name) {
  using M = Math2<Fn>;
  return {ValueOpcode::NONE, name, binaryDoubleResolve, M::point, evalFunctionBatch, M::bounds,
          evalScalarOnlyElement, 2, 2, {RAW, RAW}, M::bucket};
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
    ValueBounds out = ValueBounds::integer(~args[0].intMax, ~args[0].intMin, args[0].mayBeMissing);
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
  if (singleton(input)) {
    if (input.type == ValueType::INT64) return constantInteger(node, input.intMin, input.mayBeMissing);
    if (fitsInt64(input.doubleMin)) {
      return constantInteger(node, (int64_t)input.doubleMin, input.mayBeMissing);
    }
    if (input.minAttained && input.maxAttained) {
      return ValueBounds::invalid(node.type, BoundsInvalidity::INTEGER_OVERFLOW, input.mayBeMissing);
    }
  }
  return ValueBounds::unbounded(node.type, input.mayBeMissing);
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
  ValueBounds out = ValueBounds::floating(boundMinAsDouble(input), boundMaxAsDouble(input),
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
    ValueBounds out = ValueBounds::integer(min, max, mayBeMissing);
    out.type = node.type;
    out.minAttained = out.maxAttained = bounded(args[0]) && singleton(args[0]);
    return out;
  }
  double low = boundMinAsDouble(args[1]);
  double high = boundMinAsDouble(args[2]);
  auto clamped = [&](double value) { return std::min(std::max(value, low), high); };
  double min = bounded(args[0]) ? clamped(boundMinAsDouble(args[0])) : clamped(-INF);
  double max = bounded(args[0]) ? clamped(boundMaxAsDouble(args[0])) : clamped(INF);
  ValueBounds out = ValueBounds::floating(min, max, mayBeMissing);
  out.type = node.type;
  out.minAttained = out.maxAttained = bounded(args[0]) && singleton(args[0]);
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
inline constexpr auto LN = [](double x) { return std::log(x); };
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

constexpr ValueFunction FUNCTIONS[] = {
    unary<SIN, -1.0, 1.0>("sin"),
    unary<COS, -1.0, 1.0>("cos"),
    unary<TAN>("tan"),
    unary<ASIN, -HALF_PI, HALF_PI>("asin"),
    unary<ACOS, 0.0, std::numbers::pi>("acos"),
    unary<ATAN, -HALF_PI, HALF_PI>("atan"),
    unary<SINH>("sinh"),
    unary<COSH, 1.0, INF>("cosh"),
    unary<TANH, -1.0, 1.0>("tanh"),
    unary<ASINH>("asinh"),
    unary<ACOSH>("acosh"),
    unary<ATANH>("atanh"),
    unary<LN>("ln"),
    unary<LOG2>("log2"),
    unary<LOG10>("log10"),
    unary<EXPM1, -1.0, INF>("expm1"),
    unary<CBRT>("cbrt"),
    unary<CEIL>("ceil"),
    unary<ROUND>("round"),
    unary<RINT>("rint"),
    unary<TRUNC>("trunc"),
    unary<SIGNUM, -1.0, 1.0>("signum"),
    unary<TO_RADIANS>("to_radians"),
    unary<TO_DEGREES>("to_degrees"),
    binary<ATAN2>("atan2"),
    binary<HYPOT>("hypot"),
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
