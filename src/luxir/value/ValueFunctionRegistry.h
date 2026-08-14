#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "luxir/value/ValueTypes.h"

namespace luxir {

class BoundValueProgram;
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
  MIN,
  MAX,
  AVG,
};

struct ValueFunction {
  using ResolveType = ValueType (*)(std::span<const ValueType> args);
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

  ValueOpcode opcode = ValueOpcode::NONE;
  std::string_view name;
  ResolveType resolveType = nullptr;
  EvalPoint evalPoint = nullptr;
  EvalBatch evalBatch = nullptr;
  BoundsPropagate boundsPropagate = nullptr;
  EvalElement evalElement = nullptr;
  uint8_t minArity = 0;
  uint8_t maxArity = 0;
};

class ValueFunctionRegistry {
public:
  static const ValueFunction* find(std::string_view name);
  static std::span<const ValueFunction> entries();
};

} // namespace luxir
