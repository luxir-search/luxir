// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// let and fold: local variables and bounded loops.
//
//   let(name, value, body)
//       Evaluates `value` once and makes it available as `name` in `body`.
//
//   fold(acc, init, i, n, body)
//       acc starts as `init`; then for i = 0 .. n-1, acc becomes `body`. The
//       result is acc. `n` is a constant, so every loop ends after a fixed
//       number of steps (see the limits in ValueExprParser.h).
//
// A variable holds one number. A missing value stays missing and flows through
// the arithmetic as it does for a column; def() recovers it. The parser builds
// these nodes itself (the variable names are not expressions); this file holds
// their evaluation.
//
// Nodes: let has children {value, body}; fold has children {init, body, n}.
// node.slot is the variable (let) or accumulator (fold) slot, and
// node.counterSlot the fold counter's slot.

#include "luxir/value/ValueFunctionSupport.h"

namespace luxir {
namespace {

using namespace valuefn;

constexpr ValueFunction::Operand RAW{ValueFunction::DateUnit::RAW};
constexpr ValueFunction::Operand LAZY_RAW{ValueFunction::DateUnit::RAW, true};
constexpr ValueFunction::Operand CONSTANT_RAW{ValueFunction::DateUnit::RAW, false, true};

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

ResolvedValue foldResolve(std::span<const ResolvedValue> args) {
  return args[0];
}

ValueResult foldPoint(BoundValueProgram& program, const ValueNode& node,
                      int32_t docid, float score) {
  int64_t steps = program.program.nodes[node.children[2]].intValue;
  ValueResult acc = promote(program.evalNode(node.children[0], docid, score), node.type);
  for (int64_t i = 0; i < steps; i++) {
    program.locals[node.counterSlot] = ValueResult::integer(i);
    program.locals[node.slot] = acc;
    acc = promote(program.evalNode(node.children[1], docid, score), node.type);
  }
  return acc;
}

// With no steps the result is the initial value. Otherwise nothing is known of
// the accumulator, and the body may produce missing values.
ValueBounds foldBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  if (singleton(args[2]) && args[2].intMin == 0) return retag(args[0], node.type);
  return ValueBounds::unbounded(node.type, args[0].mayBeMissing || args[1].mayBeMissing);
}

constexpr ValueFunction FUNCTIONS[] = {
    {ValueOpcode::NONE, "let", letResolve, letPoint, evalFunctionBatch, letBounds,
     evalScalarOnlyElement, 2, 2, {RAW, RAW}},
    {ValueOpcode::NONE, "fold", foldResolve, foldPoint, evalFunctionBatch, foldBounds,
     evalScalarOnlyElement, 3, 3, {RAW, LAZY_RAW, CONSTANT_RAW}},
};

static_assert(badOperandFlags(FUNCTIONS).empty(), badOperandFlags(FUNCTIONS));

} // namespace

std::span<const ValueFunction> valueScopeFunctions() {
  return FUNCTIONS;
}

} // namespace luxir
