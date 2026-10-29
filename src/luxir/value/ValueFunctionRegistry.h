// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

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
};

struct ValueFunction {
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
  EvalBucketScalar evalBucketScalar = nullptr;
  bool combinesDates = false;

  // Normalize only date-combining functions; explicit scaling still uses raw
  // numbers. Parsers insert the conversion once, before resolving the parent.
  int dateConversionOperand(std::span<const ResolvedValue> args) const {
    if (!combinesDates || args.size() != 2
        || !valueDate(args[0].nature) || !valueDate(args[1].nature)
        || args[0].nature == args[1].nature) return -1;
    return args[0].nature == ValueNature::DATE_MS ? 0 : 1;
  }
};

class ValueFunctionRegistry {
public:
  static const ValueFunction* find(std::string_view name);
  static std::span<const ValueFunction> entries();
  static std::string arrayReducerNames();
};

} // namespace luxir
