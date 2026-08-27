#pragma once

#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

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
    ValueExprGrammar grammar(*this, *cur);
    program->rootNode = grammar.parseExpression();
    cur->skipWs();
    if (!cur->atEnd()) fail(cur->position(), "unexpected trailing input");
    if (program->leaves.empty()) {
      fail(0, "expression contains no aggregate function");
    }
    cur = nullptr;
    return program;
  }

  void enterDepth(size_t pos) {
    if (*budget <= 0) fail(pos, "expression nesting exceeds the supported depth");
    --*budget;
  }

  void leaveDepth() { ++*budget; }

  uint32_t makeUnary(char op, size_t pos, uint32_t child) {
    assert(op == '-');
    return makeUnary(*ValueFunctionRegistry::find("neg"), pos, child);
  }

  uint32_t makeUnary(const ValueFunction& function, size_t pos,
                     uint32_t child) {
    ResolvedValue resolved = resolveFunction(function, pos, {child, 0}, 1);
    AggregateNode node;
    node.kind = AggregateNodeKind::UNARY;
    node.type = valueDouble(resolved.type)
        ? BucketValueType::DOUBLE : BucketValueType::INT128;
    node.nature = resolved.nature;
    node.opcode = function.opcode;
    node.function = &function;
    node.children[0] = child;
    node.sourcePos = pos;
    return program->addNode(node);
  }

  uint32_t makeBinary(char op, size_t pos, uint32_t left, uint32_t right) {
    const char* name = nullptr;
    switch (op) {
      case '+': name = "add"; break;
      case '-': name = "sub"; break;
      case '*': name = "mul"; break;
      case '/': name = "div"; break;
      default: std::unreachable();
    }
    return makeBinary(*ValueFunctionRegistry::find(name), pos, left, right);
  }

  uint32_t makeBinary(const ValueFunction& function, size_t pos,
                      uint32_t left, uint32_t right) {
    ResolvedValue resolved = resolveFunction(function, pos, {left, right}, 2);
    AggregateNode node;
    node.kind = AggregateNodeKind::BINARY;
    node.type = valueDouble(resolved.type)
        ? BucketValueType::DOUBLE : BucketValueType::INT128;
    node.nature = resolved.nature;
    node.opcode = function.opcode;
    node.function = &function;
    node.children = {left, right};
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
        && lookup.aggregate->supports(FunctionCapability::BUCKET_AGGREGATE)) {
      return parseAggregate(*lookup.aggregate, pos);
    }
    if (lookup.value != nullptr
        && lookup.value->supports(FunctionCapability::BUCKET_SCALAR)) {
      return parseBucketFunction(grammar, *lookup.value, pos);
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
                                std::array<uint32_t, 2> children,
                                uint8_t count) {
    std::array<ResolvedValue, 2> args{};
    for (uint8_t i = 0; i < count; i++) {
      const AggregateNode& child = program->nodes[children[i]];
      args[i] = {
          child.type == BucketValueType::DOUBLE
              ? ValueType::DOUBLE : ValueType::INT64,
          child.nature};
    }
    try {
      return function.resolve(
          std::span<const ResolvedValue>(args.data(), count));
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
  uint32_t parseBucketFunction(Grammar& grammar,
                               const ValueFunction& function,
                               size_t pos) {
    ValueExprDepthGuard guard(*this, pos);

    cur->advance();
    std::array<uint32_t, 2> args{};
    uint8_t count = 0;
    cur->skipWs();
    if (!cur->consume(')')) {
      for (;;) {
        if (count == args.size()) {
          fail(cur->position(), fmt::format(
              "{}() accepts at most {} arguments", function.name,
              function.maxArity));
        }
        args[count++] = grammar.parseExpression();
        auto separator = value::lex::consumeListSeparator(*cur);
        if (separator == value::lex::ListSeparator::CLOSE) break;
        if (separator != value::lex::ListSeparator::COMMA) {
          fail(cur->position(), fmt::format(
              "expected ',' or ')' in {}(...)", function.name));
        }
        cur->skipWs();
        if (cur->peek() == ')') {
          fail(cur->position(), "trailing comma is not allowed");
        }
      }
    }
    if (count < function.minArity || count > function.maxArity) {
      fail(pos, fmt::format("{}() expects exactly {} argument{}",
                            function.name, function.minArity,
                            function.minArity == 1 ? "" : "s"));
    }
    return count == 1 ? makeUnary(function, pos, args[0])
                      : makeBinary(function, pos, args[0], args[1]);
  }

  uint32_t parseAggregate(const AggregateFunction& function, size_t pos) {
    std::string_view name = function.name;
    ValueExprDepthGuard guard(*this, pos);
    cur->advance();
    cur->skipWs();
    size_t argumentStart = cur->position();

    ValueProgram* input = nullptr;
    uint8_t argumentCount = 0;
    if (cur->peek() != ')') {
      ValueExprOptions valueOptions{opts.schema, opts.vars, budget, false};
      try {
        input = ValueExprParser(valueOptions, arena).parsePartial(*cur);
      } catch (const std::runtime_error& error) {
        throw std::runtime_error(fmt::format("op '{}': {}", opts.opName, error.what()));
      }
      argumentCount = 1;
    }
    size_t argumentEnd = cur->position();
    cur->skipWs();
    if (!cur->consume(')')) {
      fail(cur->position(), fmt::format(
          "{}() expects exactly {} argument{}", name,
          function.maxArguments, function.maxArguments == 1 ? "" : "s"));
    }
    if (argumentCount < function.minArguments
        || argumentCount > function.maxArguments) {
      fail(argumentStart, fmt::format(
          "{}() expects exactly {} argument{}", name,
          function.minArguments, function.minArguments == 1 ? "" : "s"));
    }

    if (input != nullptr && input->needsScore) {
      throw std::runtime_error(fmt::format(
          "op '{}': scores are not available in aggregate expressions",
          opts.opName));
    }
    if (input != nullptr && valueArray(input->root().type)) {
      std::string_view argument = cur->slice(argumentStart, argumentEnd);
      fail(pos, fmt::format(
          "{}() requires one scalar per document; '{}' produces a {}; "
          "reduce it per document first with avg(...), min(...), or max(...)",
          name, argument, valueTypeName(input->root().type)));
    }
    if (input != nullptr && input->root().type == ValueType::COLUMN_ONLY) {
      fail(pos, fmt::format("{}() requires a numeric expression", name));
    }

    ResolvedAggregate resolved;
    try {
      resolved = function.resolve(input == nullptr ? nullptr : &input->root());
    } catch (const std::exception& error) {
      fail(pos, error.what());
    }

    AggregateLeaf leaf;
    leaf.input = input;
    leaf.resolved = resolved;
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
};

} // namespace luxir
