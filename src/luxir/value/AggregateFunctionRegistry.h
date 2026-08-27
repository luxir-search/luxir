#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "luxir/value/AggregateExpr.h"
#include "luxir/value/ValueFunctionRegistry.h"

namespace luxir {

enum class AggregateOpcode : uint8_t {
  AVG,
  SUM,
  MIN,
  MAX,
};

struct AggregateFunction {
  using Resolve = ResolvedAggregate (*)(const ValueNode* input);

  AggregateOpcode opcode;
  std::string_view name;
  uint8_t minArguments = 0;
  uint8_t maxArguments = 0;
  Resolve resolve = nullptr;
  uint8_t capabilities = functionCapabilities(
      FunctionCapability::BUCKET_AGGREGATE);

  bool supports(FunctionCapability capability) const {
    return (capabilities & (uint8_t)capability) != 0;
  }
};

struct ExpressionFunctionLookup {
  const ValueFunction* value = nullptr;
  const AggregateFunction* aggregate = nullptr;

  bool known() const { return value != nullptr || aggregate != nullptr; }
};

class ExpressionFunctionRegistry {
public:
  static ExpressionFunctionLookup find(std::string_view name);
};

class AggregateFunctionRegistry {
public:
  static const AggregateFunction* find(std::string_view name);
  static std::span<const AggregateFunction> entries();
};

} // namespace luxir
