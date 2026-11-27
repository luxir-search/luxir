// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "luxir/value/AggregateExpr.h"

namespace luxir {

// Mergeable sketch states: percentile_state and cardinality_state return the
// serialized t-digest and distinct-count sketch (TDigest::writeState,
// HyperLogLog::writeState) instead of a number. They live in their own
// translation unit so their template instances do not take the inlining
// budget of the registry's accumulate loops.
ResolvedAggregate resolvePercentileState(
    const ValueNode* input, std::span<AggregateConstant> arguments);
ResolvedAggregate resolveCardinalityState(
    const ValueNode* input, std::span<AggregateConstant> arguments);
AggregateConstant canonicalPercentileStateArgument(
    std::span<const AggregateConstant> arguments, uint8_t index);

// Shared argument checks; each throws std::runtime_error naming `function`.
void requireNumericInput(const ValueNode* input, std::string_view function);
void validateCardinality(const ValueNode* input, std::span<const AggregateConstant> arguments,
                         std::string_view function);

} // namespace luxir
