// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "luxir/value/ValueTypes.h"

namespace luxir {

class BoundValueProgram;
struct BucketScalar;
struct ValueNode;

enum class ValueOpcode : uint8_t {
  NONE,
  DEF,
  ADD,
  SUB,
  MUL,
  DIV,
  NEG,
  ABS,
  SQRT,
  LOG,
  LOG1P,
  FLOOR,
  MIN,
  MAX,
  AVG,
  EXP,
  POW,
  LT,
  LE,
  GT,
  GE,
  EQ,
  NE,
  AND,
  OR,
  NOT,
  IF,
  RANDOM,
  GEO_DISTANCE,
};

// Most children any one value-function node has (if(cond, then, else)).
inline constexpr size_t VALUE_MAX_ARITY = 3;

struct ValueFunction {
  enum class DateUnit : uint8_t { UNSET, SHARED, RAW };
  struct Operand {
    DateUnit unit = DateUnit::UNSET;
    bool lazy = false;
  };

  using Resolve = ResolvedValue (*)(std::span<const ResolvedValue> args);
  using EvalPoint = ValueResult (*)(BoundValueProgram& program, const ValueNode& node,
                                    int32_t docid, float score);
  using EvalBatch = void (*)(BoundValueProgram& program, const ValueNode& node,
                             std::span<const int32_t> docids,
                             std::span<const float> scores,
                             std::span<ValueResult> results);
  using BoundsPropagate = ValueBounds (*)(const ValueNode& node,
                                          std::span<const ValueBounds> args);
  using EvalElement = ValueResult (*)(BoundValueProgram& program, const ValueNode& node,
                                      const ValueArrayRef& array, int64_t index);
  using EvalBucketScalar = BucketScalar (*)(
      std::span<const BucketScalar> args, ValueType type,
      ValueNature nature);

  ValueOpcode opcode = ValueOpcode::NONE;
  std::string_view name;
  Resolve resolve = nullptr;
  EvalPoint evalPoint = nullptr;
  EvalBatch evalBatch = nullptr;
  BoundsPropagate boundsPropagate = nullptr;
  EvalElement evalElement = nullptr;
  uint8_t minArity = 0;
  uint8_t maxArity = 0;
  std::array<Operand, VALUE_MAX_ARITY> operands{};
  EvalBucketScalar evalBucketScalar = nullptr;

  // Strict INVALID operands make the result INVALID; only lazy operands reach
  // boundsPropagate possibly INVALID.
  ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds> args) const;

  // Shared DATE operands use ns when any of them uses ns. RAW operands keep
  // their native numeric representation.
  std::array<bool, VALUE_MAX_ARITY> dateConversionOperands(
      std::span<const ResolvedValue> args) const {
    bool nanos = false;
    for (size_t i = 0; i < args.size(); i++) {
      if (operands[i].unit == DateUnit::SHARED
          && args[i].nature == ValueNature::DATE_NS) nanos = true;
    }
    std::array<bool, VALUE_MAX_ARITY> convert{};
    for (size_t i = 0; i < args.size(); i++) {
      convert[i] = nanos && operands[i].unit == DateUnit::SHARED
          && args[i].nature == ValueNature::DATE_MS;
    }
    return convert;
  }
};

class ValueFunctionRegistry {
public:
  static const ValueFunction* find(std::string_view name);
  static std::span<const ValueFunction> entries();
  static std::string arrayReducerNames();
};

} // namespace luxir
