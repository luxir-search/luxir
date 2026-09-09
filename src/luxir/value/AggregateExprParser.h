// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/Schema.h"
#include "luxir/util/Cursor.h"
#include "luxir/util/proto.h"
#include "luxir/value/AggregateExpr.h"
#include "luxir/value/AggregateFunctionRegistry.h"
#include "luxir/value/ValueExprGrammar.h"
#include "luxir/value/ValueExprParser.h"
#include "luxir/value/ValueLex.h"

namespace luxir {

struct AggregateExprOptions {
  Schema* schema = nullptr;
  api::map_view<std::string_view, ::hpp_proto::indirect_view<api::Val>> vars;
  std::string_view opName;
  int* nestingBudget = nullptr;
  FieldResolver* fieldResolver = nullptr;
};

class AggregateExprParser {
  static constexpr int DEFAULT_NESTING_BUDGET = 128;

  const AggregateExprOptions& opts;
  google::protobuf::Arena& arena;
  Cursor* cur = nullptr;
  AggregateProgram* program = nullptr;
  int localBudget = DEFAULT_NESTING_BUDGET;
  int* budget;

public:
  using Node = uint32_t;

  AggregateExprParser(const AggregateExprOptions& opts,
                      google::protobuf::Arena& arena)
      : opts(opts), arena(arena),
        budget(opts.nestingBudget ? opts.nestingBudget : &localBudget) {}

  AggregateProgram* parse(std::string_view expression) {
    if (opts.schema == nullptr) {
      throw std::runtime_error("aggregate expressions require a schema");
    }
    Cursor cursor(expression);
    cur = &cursor;
    program = arenaCreate<AggregateProgram>(arena, arena);
    cur->skipWs();
    if (cur->atEnd()) fail(0, "empty aggregate expression");
    ValueExprGrammar grammar(*this, *cur, *budget);
    program->rootNode = grammar.parseExpression();
    cur->skipWs();
    if (!cur->atEnd()) fail(cur->position(), "unexpected trailing input");
    if (program->leaves.empty()) {
      fail(0, "expression contains no aggregate function");
    }
    cur = nullptr;
    return program;
  }

  uint32_t makeFunction(std::string_view name, size_t pos,
                        std::span<const uint32_t> children) {
    const ValueFunction* function = ValueFunctionRegistry::find(name);
    assert(function != nullptr && function->evalBucketScalar != nullptr);
    ResolvedValue resolved = resolveFunction(*function, pos, children);
    AggregateNode node;
    node.kind = children.size() == 1
        ? AggregateNodeKind::UNARY : AggregateNodeKind::BINARY;
    node.type = valueDouble(resolved.type)
        ? BucketValueType::DOUBLE : BucketValueType::INT128;
    node.nature = resolved.nature;
    node.function = function;
    std::copy(children.begin(), children.end(), node.children.begin());
    node.sourcePos = pos;
    return program->addNode(node);
  }

  template <class Grammar>
  uint32_t parseAtom(Grammar& grammar) {
    cur->skipWs();
    size_t pos = cur->position();
    char c = cur->peek();
    if (c == '$') return parseVariable();
    if (c == '+' || c == '-' || c == '.' || value::lex::digit(c)) {
      return parseNumber();
    }
    std::string_view name = value::lex::scanIdentifier(*cur);
    if (name.empty()) {
      fail(pos, "expected a number, variable, aggregate call, or parenthesized expression");
    }
    if (cur->peek() != '(') {
      fail(pos, fmt::format("bucket expression leaf '{}' must be an aggregate call", name));
    }
    ExpressionFunctionLookup lookup = ExpressionFunctionRegistry::find(name);
    if (lookup.aggregate != nullptr
        && !(lookup.value != nullptr
             && lookup.value->evalBucketScalar != nullptr
             && grammar.hasTopLevelComma())) {
      return parseAggregate(grammar, *lookup.aggregate, pos);
    }
    if (lookup.value != nullptr
        && lookup.value->evalBucketScalar != nullptr) {
      std::array<uint32_t, 2> args{};
      uint8_t count = grammar.parseArguments(
          name, pos, lookup.value->minArity, lookup.value->maxArity,
          [&](uint8_t argument) {
            args[argument] = grammar.parseExpression();
          });
      return makeFunction(name, pos,
                          std::span<const uint32_t>(args.data(), count));
    }
    if (lookup.value != nullptr) {
      fail(pos, fmt::format(
          "{}() is a per-document value function and is not valid on bucket values",
          name));
    }
    fail(pos, fmt::format("unknown expression function '{}()'", name));
  }

  [[noreturn]] void fail(size_t pos, std::string_view message) const {
    throw std::runtime_error(fmt::format(
        "op '{}': aggregate expression parse error at byte {}: {} "
        "(context: \"{}\")",
        opts.opName, pos, message, cur->errorContext(pos)));
  }

private:
  ResolvedValue resolveFunction(const ValueFunction& function, size_t pos,
                                std::span<const uint32_t> children) {
    std::array<ResolvedValue, 2> args{};
    for (size_t i = 0; i < children.size(); i++) {
      const AggregateNode& child = program->nodes[children[i]];
      args[i] = {
          child.type == BucketValueType::DOUBLE
              ? ValueType::DOUBLE : ValueType::INT64,
          child.nature};
    }
    try {
      return function.resolve(
          std::span<const ResolvedValue>(args.data(), children.size()));
    } catch (const std::exception& error) {
      fail(pos, error.what());
    }
  }

  uint32_t parseNumber() {
    size_t pos = cur->position();
    value::lex::NumericLiteral literal = value::lex::parseNumericLiteral(
        *cur, [&](size_t at, std::string_view message) { fail(at, message); });

    AggregateNode node;
    node.kind = AggregateNodeKind::CONSTANT;
    node.sourcePos = pos;
    if (literal.floating) {
      node.type = BucketValueType::DOUBLE;
      node.doubleValue = literal.doubleValue;
    } else {
      node.intValue = literal.intValue;
    }
    return program->addNode(node);
  }

  uint32_t parseVariable() {
    size_t pos = cur->position();
    std::string_view name = value::lex::scanVariable(*cur);
    if (name.empty()) fail(pos, "'$' must be followed by a variable name");
    const auto* view = opts.vars.find(name);
    if (view == nullptr) fail(pos, fmt::format("undefined variable ${}", name));

    const api::Val& value = **view;
    AggregateNode node;
    node.kind = AggregateNodeKind::CONSTANT;
    node.sourcePos = pos;
    if (const auto* integer = std::get_if<int64_t>(&value.kind)) {
      node.intValue = *integer;
    } else if (const auto* floating = std::get_if<double>(&value.kind)) {
      if (!std::isfinite(*floating)) {
        fail(pos, fmt::format("variable ${} contains NaN or infinity", name));
      }
      node.type = BucketValueType::DOUBLE;
      node.doubleValue = *floating;
    } else if (const auto* floating = std::get_if<float>(&value.kind)) {
      if (!std::isfinite(*floating)) {
        fail(pos, fmt::format("variable ${} contains NaN or infinity", name));
      }
      node.type = BucketValueType::DOUBLE;
      node.doubleValue = (double)*floating;
    } else {
      fail(pos, fmt::format("bucket variable ${} must be a numeric scalar", name));
    }
    return program->addNode(node);
  }

  template <class Grammar>
  uint32_t parseAggregate(Grammar& grammar,
                          const AggregateFunction& function, size_t pos) {
    std::string_view name = function.name;
    ValueProgram* input = nullptr;
    size_t argumentStart = 0;
    size_t argumentEnd = 0;
    std::vector<AggregateConstant> trailing;
    grammar.parseArguments(
        name, pos, function.minArguments, function.maxArguments,
        [&](uint8_t argument) {
          if (argument == 0) {
            argumentStart = cur->position();
            ValueExprOptions valueOptions{opts.schema, opts.vars, budget, false, opts.fieldResolver, opts.opName};
            try {
              input = ValueExprParser(valueOptions, arena).parsePartial(*cur);
            } catch (const std::runtime_error& error) {
              throw std::runtime_error(fmt::format(
                  "op '{}': {}", opts.opName, error.what()));
            }
            argumentEnd = cur->position();
          } else {
            trailing.push_back(parseAggregateConstant());
          }
        });

    if (input != nullptr && input->needsScore) {
      throw std::runtime_error(fmt::format(
          "op '{}': scores are not available in aggregate expressions",
          opts.opName));
    }
    if (input != nullptr && valueArray(input->root().type)) {
      std::string_view argument = cur->slice(argumentStart, argumentEnd);
      fail(pos, fmt::format(
          "{}() requires one scalar per document; '{}' produces a {}; "
          "reduce it per document first with {}",
          name, argument, valueTypeName(input->root().type),
          ValueFunctionRegistry::arrayReducerNames()));
    }
    if (input != nullptr && input->root().type == ValueType::COLUMN_ONLY) {
      fail(pos, fmt::format("{}() requires a numeric expression", name));
    }

    ResolvedAggregate resolved;
    try {
      resolved = function.resolve(
          input == nullptr ? nullptr : &input->root(), trailing);
    } catch (const std::exception& error) {
      fail(pos, error.what());
    }

    AggregateLeaf leaf;
    leaf.input = input;
    leaf.resolved = resolved;
    leaf.arguments = program->copyConstants(trailing);
    leaf.stateOffset = program->stateBytes;
    program->stateBytes += leaf.resolved.state.bytes;
    uint32_t leafIndex = (uint32_t)program->leaves.size();
    program->leaves.push_back(leaf);

    AggregateNode node;
    node.kind = AggregateNodeKind::AGGREGATE;
    node.type = resolved.type;
    node.nature = resolved.nature;
    node.aggregate = leafIndex;
    node.sourcePos = pos;
    return program->addNode(node);
  }

  AggregateConstant parseAggregateConstant() {
    cur->skipWs();
    size_t pos = cur->position();
    if (cur->peek() == '$') {
      std::string_view name = value::lex::scanVariable(*cur);
      if (name.empty()) fail(pos, "'$' must be followed by a variable name");
      const auto* view = opts.vars.find(name);
      if (view == nullptr) fail(pos, fmt::format("undefined variable ${}", name));
      const api::Val& value = **view;
      if (const auto* integer = std::get_if<int64_t>(&value.kind)) {
        return {ValueType::INT64, *integer, 0.0};
      }
      if (const auto* floating = std::get_if<double>(&value.kind)) {
        if (!std::isfinite(*floating)) {
          fail(pos, fmt::format("variable ${} contains NaN or infinity", name));
        }
        return {ValueType::DOUBLE, 0, *floating};
      }
      if (const auto* floating = std::get_if<float>(&value.kind)) {
        if (!std::isfinite(*floating)) {
          fail(pos, fmt::format("variable ${} contains NaN or infinity", name));
        }
        return {ValueType::DOUBLE, 0, (double)*floating};
      }
      fail(pos, fmt::format(
          "aggregate parameter ${} must be a numeric scalar", name));
    }
    if (cur->peek() == '+' || cur->peek() == '-' || cur->peek() == '.'
        || value::lex::digit(cur->peek())) {
      value::lex::NumericLiteral literal = value::lex::parseNumericLiteral(
          *cur, [&](size_t at, std::string_view message) { fail(at, message); });
      return literal.floating
          ? AggregateConstant{ValueType::DOUBLE, 0, literal.doubleValue}
          : AggregateConstant{ValueType::INT64, literal.intValue, 0.0};
    }
    fail(pos, "aggregate trailing arguments must be numeric constants or scalar variables");
  }
};

} // namespace luxir
