// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "luxir/value/AggregateExpr.h"
#include "luxir/value/ValueFunctionRegistry.h"

namespace luxir {

struct AggregateFunction {
  using Resolve = ResolvedAggregate (*)(
      const ValueNode* input,
      std::span<AggregateConstant> trailingArguments);
  using CanonicalArgument = AggregateConstant (*)(
      std::span<const AggregateConstant> arguments, uint8_t index);

  std::string_view name;
  uint8_t minArguments = 0;
  uint8_t maxArguments = 0;
  Resolve resolve = nullptr;
  // Set-valued aggregates flatten arrays and accept ordinal columns.
  bool acceptsValues = false;
  // Bits name trailing arguments used only by finish, starting at zero.
  uint32_t readoutArguments = 0;
  CanonicalArgument canonicalArgument = nullptr;

  bool sameStateArguments(std::span<const AggregateConstant> left,
                          std::span<const AggregateConstant> right) const;
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
