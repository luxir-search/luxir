#pragma once

#include <cstdint>
#include <span>
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

enum class FunctionCapability : uint8_t {
  DOCUMENT_VALUE = 1,
  BUCKET_SCALAR = 2,
  BUCKET_AGGREGATE = 4,
};

constexpr uint8_t functionCapabilities(FunctionCapability a) {
  return (uint8_t)a;
}

constexpr uint8_t functionCapabilities(FunctionCapability a,
                                       FunctionCapability b) {
  return (uint8_t)a | (uint8_t)b;
}

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
  uint8_t capabilities = functionCapabilities(FunctionCapability::DOCUMENT_VALUE);
  EvalBucketScalar evalBucketScalar = nullptr;

  bool supports(FunctionCapability capability) const {
    if (capability == FunctionCapability::BUCKET_SCALAR) {
      return evalBucketScalar != nullptr;
    }
    return (capabilities & (uint8_t)capability) != 0;
  }
};

class ValueFunctionRegistry {
public:
  static const ValueFunction* find(std::string_view name);
  static std::span<const ValueFunction> entries();
};

} // namespace luxir
