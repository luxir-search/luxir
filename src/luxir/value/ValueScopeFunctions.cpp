// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Evaluation of let bindings.

#include "luxir/value/ValueFunctionSupport.h"

namespace luxir {
namespace {

using namespace valuefn;

constexpr ValueFunction::Operand RAW{ValueFunction::DateUnit::RAW};

ResolvedValue letResolve(std::span<const ResolvedValue> args) {
  return args[1];
}

ValueResult letPoint(BoundValueProgram& program, const ValueNode& node,
                     int32_t docid, float score) {
  program.locals[node.slot] = program.evalNode(node.children[0], docid, score);
  return program.evalNode(node.children[1], docid, score);
}

ValueBounds letBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  return retag(args[1], node.type);
}

constexpr ValueFunction FUNCTIONS[] = {
    {ValueOpcode::NONE, "let", letResolve, letPoint, evalFunctionBatch, letBounds,
     evalScalarOnlyElement, 2, 2, {RAW, RAW}},
};

static_assert(badOperandFlags(FUNCTIONS).empty(), badOperandFlags(FUNCTIONS));

} // namespace

std::span<const ValueFunction> valueScopeFunctions() {
  return FUNCTIONS;
}

} // namespace luxir
