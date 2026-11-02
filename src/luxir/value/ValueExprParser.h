// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "luxir/api/luxir_types.hpp"
#include "luxir/query/FieldResolver.h"
#include "luxir/util/Cursor.h"
#include "luxir/util/proto.h"
#include "luxir/value/AggregateFunctionRegistry.h"
#include "luxir/value/ValueExpr.h"
#include "luxir/value/ValueExprGrammar.h"
#include "luxir/value/ValueLex.h"

namespace luxir {

struct ValueExprOptions {
  Schema* schema = nullptr;
  api::map_view<std::string_view, ::hpp_proto::indirect_view<api::Val>> vars;
  int* nestingBudget = nullptr;
  bool sortIntrinsics = true;
  FieldResolver* fieldResolver = nullptr;
  std::string_view opName = {};
};

class ValueExprParser {
  class ValueConstantAnalysis {
    std::vector<std::optional<ValueBounds>> bounds;
    std::vector<ValueType> types;

  public:
    void clear() {
      bounds.clear();
      types.clear();
    }

    void append(const ValueNode& node, std::optional<ValueBounds> literal = std::nullopt) {
      uint32_t index = (uint32_t)bounds.size();
      std::optional<ValueBounds> result = literal;
      if (node.kind == ValueNodeKind::CONSTANT || node.kind == ValueNodeKind::VARIABLE) {
        if (node.type == ValueType::INT64) {
          result = ValueBounds::integer(node.intValue, node.intValue);
        } else if (node.type == ValueType::DOUBLE) {
          result = ValueBounds::floating(node.doubleValue, node.doubleValue);
        }
      } else if (node.kind == ValueNodeKind::FUNCTION) {
        std::array<ValueBounds, VALUE_MAX_ARITY> children;
        bool independent = true;
        for (uint8_t child = 0; child < node.childCount; child++) {
          const auto& childBounds = bounds[node.children[child]];
          if (!childBounds.has_value()) {
            independent = false;
            children[child] = ValueBounds::unbounded(
                types[node.children[child]], true);
            continue;
          }
          children[child] = *childBounds;
        }
        ValueBounds value = node.function->bounds(
            node, std::span<const ValueBounds>(children.data(), node.childCount));
        if (value.certainty == BoundsCertainty::INVALID) {
          if (value.invalidNode == UINT32_MAX) value.invalidNode = index;
          result = value;
        } else if (independent) {
          result = value;
        }
      }
      bounds.push_back(result);
      types.push_back(node.type);
    }

    std::optional<ValueResult> scalar(uint32_t index) const {
      const auto& rootBounds = bounds[index];
      ValueType rootType = rootBounds.has_value() ? rootBounds->type : ValueType::COLUMN_ONLY;
      if (!rootBounds.has_value()
          || rootBounds->certainty != BoundsCertainty::BOUNDED
          || rootBounds->mayBeMissing || rootBounds->alwaysMissing
          || valueArray(rootType) || rootType == ValueType::COLUMN_ONLY) {
        return std::nullopt;
      }
      if (rootType == ValueType::INT64
          && rootBounds->intMin == rootBounds->intMax) {
        return ValueResult::integer(rootBounds->intMin);
      }
      if (rootType == ValueType::DOUBLE
          && rootBounds->doubleMin == rootBounds->doubleMax
          && std::isfinite(rootBounds->doubleMin)) {
        return ValueResult::floating(rootBounds->doubleMin);
      }
      return std::nullopt;
    }
  };

  static constexpr int DEFAULT_NESTING_BUDGET = 128;

  const ValueExprOptions& opts;
  google::protobuf::Arena& arena;
  Cursor* cur = nullptr;
  ValueProgram* program = nullptr;
  ValueConstantAnalysis constants;
  int localBudget = DEFAULT_NESTING_BUDGET;
  int* budget;

public:
  using Node = uint32_t;

  ValueExprParser(const ValueExprOptions& opts, google::protobuf::Arena& arena)
      : opts(opts), arena(arena), budget(opts.nestingBudget ? opts.nestingBudget : &localBudget) {}

  ValueProgram* parse(std::string_view expression) {
    Cursor cursor(expression);
    ValueProgram* result = parsePartial(cursor);
    cursor.skipWs();
    if (!cursor.atEnd()) fail(cursor.position(), "unexpected trailing input");
    cur = nullptr;
    return result;
  }

  // Parse one expression from the cursor's current absolute position and stop
  // before a caller-owned comma or close parenthesis. The caller and this
  // parser therefore share source positions without substring translation.
  ValueProgram* parsePartial(Cursor& cursor) {
    if (opts.schema == nullptr) throw std::runtime_error("ValueExpr requires a schema");
    cur = &cursor;
    program = arenaCreate<ValueProgram>(arena, arena);
    constants.clear();
    cur->skipWs();
    if (cur->atEnd()) fail(cur->position(), "empty value expression");
    ValueExprGrammar grammar(*this, *cur, *budget);
    program->rootNode = grammar.parseExpression();
    if (program->root().nature == ValueNature::GEO_POINT) {
      fail(program->root().sourcePos, "GEO_POINT is not numeric; use geo_distance()");
    }
    program->constantScalar = constants.scalar(program->rootNode);
    return program;
  }

  uint32_t makeFunction(std::string_view name, size_t pos,
                        std::span<const uint32_t> args) {
    const ValueFunction* function = ValueFunctionRegistry::find(name);
    assert(function != nullptr);
    std::array<uint32_t, VALUE_MAX_ARITY> children{};
    assert(args.size() <= children.size());
    std::copy(args.begin(), args.end(), children.begin());
    return appendFunction(function, pos, children, (uint8_t)args.size());
  }

  template <class Grammar>
  uint32_t parseAtom(Grammar& grammar) {
    return parseValue(grammar);
  }

  [[noreturn]] void fail(size_t pos, std::string_view message) const {
    throw std::runtime_error(fmt::format(
        "value expression parse error at byte {}: {} (context: \"{}\")",
        pos, message, cur->errorContext(pos)));
  }

private:
  uint32_t append(ValueNode node) {
    program->addNode(node);
    uint32_t index = (uint32_t)program->nodes.size() - 1;
    constants.append(node, node.kind == ValueNodeKind::CONSTANT || node.kind == ValueNodeKind::VARIABLE
        ? std::optional<ValueBounds>(program->constantBounds(index)) : std::nullopt);
    return index;
  }

  template <class Grammar>
  uint32_t parseValue(Grammar& grammar) {
    cur->skipWs();
    size_t pos = cur->position();
    char c = cur->peek();
    if (c == '$') return parseVariable();
    if (c == '"' || c == '\'') {
      fail(pos, "a quoted string is only valid as the argument of col(\"name\")");
    }
    if (c == '+' || c == '-' || c == '.' || value::lex::digit(c)) {
      return parseNumber();
    }
    std::string_view name = value::lex::scanIdentifier(*cur);
    if (name.empty()) fail(pos, "expected a number, numeric column, variable, score, or function");
    if (cur->peek() == '(') return parseCall(grammar, name, pos);
    if (name == "score" || (opts.sortIntrinsics && name == "_score_")) {
      ValueNode node;
      node.kind = ValueNodeKind::SCORE;
      node.type = ValueType::DOUBLE;
      node.sourcePos = pos;
      node.text = program->copyString(name);
      program->needsScore = true;
      return append(node);
    }
    if (opts.sortIntrinsics && name == "_docid_") {
      ValueNode node;
      node.kind = ValueNodeKind::DOCID;
      node.type = ValueType::INT64;
      node.sourcePos = pos;
      node.text = program->copyString(name);
      return append(node);
    }
    return parseColumn(name, pos);
  }

  uint32_t parseNumber() {
    size_t pos = cur->position();
    value::lex::NumericLiteral literal = value::lex::parseNumericLiteral(
        *cur, [&](size_t at, std::string_view message) { fail(at, message); });

    ValueNode node;
    node.kind = ValueNodeKind::CONSTANT;
    node.sourcePos = pos;
    node.text = program->copyString(literal.text);
    if (literal.floating) {
      node.type = ValueType::DOUBLE;
      node.doubleValue = literal.doubleValue;
    } else {
      node.type = ValueType::INT64;
      node.intValue = literal.intValue;
    }
    return append(node);
  }

  uint32_t parseVariable() {
    size_t pos = cur->position();
    std::string_view name = value::lex::scanVariable(*cur);
    if (name.empty()) fail(pos, "'$' must be followed by a variable name");
    const auto* view = opts.vars.find(name);
    if (view == nullptr) {
      fail(pos, fmt::format("undefined variable ${}", name));
    }
    const api::Val& value = **view;
    ValueNode node;
    node.kind = ValueNodeKind::VARIABLE;
    node.sourcePos = pos;
    node.text = program->copyString(name);
    if (const auto* integer = std::get_if<int64_t>(&value.kind)) {
      node.type = ValueType::INT64;
      node.intValue = *integer;
    } else if (const auto* floating = std::get_if<double>(&value.kind)) {
      requireFinite(*floating, pos, name);
      node.type = ValueType::DOUBLE;
      node.doubleValue = *floating;
    } else if (const auto* floating = std::get_if<float>(&value.kind)) {
      requireFinite(*floating, pos, name);
      node.type = ValueType::DOUBLE;
      node.doubleValue = (double)*floating;
    } else if (const auto* integers = std::get_if<api::ArrInt>(&value.kind)) {
      node.type = ValueType::INT64_ARRAY;
      node.arrayOffset = (uint32_t)program->intArrays.size();
      node.arraySize = (uint32_t)integers->v.size();
      program->intArrays.insert(program->intArrays.end(), integers->v.begin(), integers->v.end());
    } else if (const auto* values = std::get_if<api::ArrDouble>(&value.kind)) {
      node.type = ValueType::DOUBLE_ARRAY;
      appendDoubleArray(node, values->v, pos, name);
    } else if (const auto* values = std::get_if<api::ArrFloat>(&value.kind)) {
      node.type = ValueType::DOUBLE_ARRAY;
      node.arrayOffset = (uint32_t)program->doubleArrays.size();
      node.arraySize = (uint32_t)values->v.size();
      for (float number : values->v) {
        requireFinite(number, pos, name);
        program->doubleArrays.push_back((double)number);
      }
    } else {
      fail(pos, fmt::format("variable ${} is not a numeric scalar or array", name));
    }
    return append(node);
  }

  template <class Number>
  void requireFinite(Number number, size_t pos, std::string_view name) {
    if (!std::isfinite(number)) {
      fail(pos, fmt::format("variable ${} contains NaN or infinity", name));
    }
  }

  void appendDoubleArray(ValueNode& node, std::span<const double> values,
                         size_t pos, std::string_view name) {
    node.arrayOffset = (uint32_t)program->doubleArrays.size();
    node.arraySize = (uint32_t)values.size();
    for (double number : values) {
      requireFinite(number, pos, name);
      program->doubleArrays.push_back(number);
    }
  }

  uint32_t parseColumn(std::string_view name, size_t pos) {
    ResolvedFieldHandle target;
    try {
      target = opts.fieldResolver ? opts.fieldResolver->resolve(name, OpClass::VALUE, opts.opName)
          : opts.schema->resolveFor(name, OpClass::VALUE);
    } catch (const RequestError& e) {
      fail(pos, e.what());
    }
    FieldType* field = target.fieldType;
    if (field->type() == FieldType::TEXT) {
      fail(pos, fmt::format("field '{}' is TEXT and has no value column; use a string variant", name));
    }
    bool numeric = field->type() == FieldType::INT || field->type() == FieldType::DATE ||
                   field->type() == FieldType::FLOAT || field->type() == FieldType::DOUBLE;
    bool stringSortable = field->type() == FieldType::ID || field->type() == FieldType::STRING;
    bool geo = field->type() == FieldType::GEO_POINT;
    if (!numeric && !stringSortable && !geo) fail(pos, fmt::format("field '{}' is not sortable", name));
    if (!field->hasColumn()) {
      fail(pos, fmt::format("field '{}' has no column values", name));
    }

    ValueNode node;
    node.kind = ValueNodeKind::COLUMN;
    node.columnType = field->type();
    node.columnMultiValued = field->multiValued();
    node.columnIndexed = field->indexed();
    if (!numeric && !geo) {
      node.type = ValueType::COLUMN_ONLY;
    } else {
      bool floating = field->type() == FieldType::FLOAT || field->type() == FieldType::DOUBLE;
      node.type = field->multiValued()
          ? (floating ? ValueType::DOUBLE_ARRAY : ValueType::INT64_ARRAY)
          : (floating ? ValueType::DOUBLE : ValueType::INT64);
      node.nature = field->type() == FieldType::DATE
          ? dateValueNature(DateFieldType::unitOf(*field))
          : geo ? ValueNature::GEO_POINT : ValueNature::NUMBER;
    }
    node.sourcePos = pos;
    node.text = program->copyString(target.physicalName);
    return append(node);
  }

  template <class Grammar>
  uint32_t parseCall(Grammar& grammar, std::string_view name, size_t pos) {
    if (name == "col") return parseExplicitColumn(grammar, pos);
    if (name == "gauss" || name == "linear" || name == "exp") {
      return parseDecayOrExp(grammar, name, pos);
    }
    ExpressionFunctionLookup lookup = ExpressionFunctionRegistry::find(name);
    const ValueFunction* function = lookup.value;
    if (function == nullptr) {
      if (lookup.aggregate != nullptr) {
        fail(pos, fmt::format(
            "{}() is a bucket aggregate and is not valid in a per-document value expression",
            name));
      }
      fail(pos, fmt::format("unknown value function '{}()'", name));
    }

    std::array<uint32_t, VALUE_MAX_ARITY> args{};
    uint8_t count = grammar.parseArguments(
        name, pos, function->minArity, function->maxArity,
        [&](uint8_t argument) {
          args[argument] = grammar.parseExpression();
        });
    return appendFunction(function, pos, args, count);
  }

  template <class Grammar>
  uint32_t parseExplicitColumn(Grammar& grammar, size_t pos) {
    std::string_view name;
    grammar.parseArguments("col", pos, 1, 1, [&](uint8_t argument) {
      unused(argument);
      cur->skipWs();
      if (cur->peek() != '"' && cur->peek() != '\'') {
        fail(cur->position(),
             "col() expects one quoted field name, for example col(\"price_i\")");
      }
      name = value::lex::scanQuoted(
          *cur, program->resource,
          [&](size_t at, std::string_view message) { fail(at, message); });
    });
    return parseColumn(name, pos);
  }

  uint32_t appendFunction(const ValueFunction* function, size_t pos,
                          std::array<uint32_t, VALUE_MAX_ARITY> args, uint8_t count) {
    assert(function != nullptr);
    std::array<ResolvedValue, VALUE_MAX_ARITY> resolvedArgs{};
    for (uint8_t i = 0; i < count; i++) {
      const ValueNode& arg = program->nodes[args[i]];
      resolvedArgs[i] = {arg.type, arg.nature};
    }
    for (uint8_t i = 0; i < count; i++) {
      if (function->operands[i].constant && !foldConstant(args[i])) {
        fail(program->nodes[args[i]].sourcePos, fmt::format(
            "{}() argument {} must be a constant numeric scalar", function->name, i + 1));
      }
    }
    auto conversions = function->dateConversionOperands(
        std::span<const ResolvedValue>(resolvedArgs.data(), count));
    for (size_t convert = 0; convert < count; convert++) {
      if (!conversions[convert]) continue;
      ValueNode scale;
      scale.intValue = 1000000;
      scale.sourcePos = pos;
      uint32_t factor = append(scale);
      args[convert] = appendFunction(ValueFunctionRegistry::find("mul"), pos,
                                     {args[convert], factor}, 2);
      program->nodes[args[convert]].nature = ValueNature::DATE_NS;
      resolvedArgs[convert].nature = ValueNature::DATE_NS;
    }

    ValueNode node;
    node.kind = ValueNodeKind::FUNCTION;
    node.opcode = function->opcode;
    node.function = function;
    node.children = args;
    node.childCount = count;
    node.sourcePos = pos;
    node.text = function->name;
    try {
      ResolvedValue resolved = function->resolveOperands(
          std::span<const ResolvedValue>(resolvedArgs.data(), count));
      node.type = resolved.type;
      node.nature = resolved.nature;
      if (function->validate != nullptr) {
        std::array<const ValueNode*, VALUE_MAX_ARITY> operands{};
        for (uint8_t i = 0; i < count; i++) operands[i] = &program->nodes[args[i]];
        function->validate({operands.data(), count});
      }
    } catch (const std::exception& error) {
      fail(pos, fmt::format("{}(): {}", function->name, error.what()));
    }
    if (node.opcode == ValueOpcode::RANDOM) {
      // ES random_score without a seed: a fresh seed for every request.
      node.intValue = count == 0
          ? (int64_t)(((uint64_t)std::random_device{}() << 32) | std::random_device{}())
          : program->nodes[args[0]].intValue;
      if (count != 0 && program->nodes[args[0]].type != ValueType::INT64) {
        fail(pos, "random() seed must be an integer constant expression");
      }
    }
    return append(node);
  }

  bool foldConstant(uint32_t index) {
    auto value = constants.scalar(index);
    if (!value.has_value()) return false;
    ValueNode& node = program->nodes[index];
    ValueNode literal;
    literal.type = value->type;
    literal.nature = node.nature;
    literal.sourcePos = node.sourcePos;
    literal.text = node.text;
    literal.intValue = value->intValue;
    literal.doubleValue = value->doubleValue;
    node = literal;
    return true;
  }

  double constantArg(uint32_t index, std::string_view function,
                     std::string_view argument) {
    if (!foldConstant(index)) {
      fail(program->nodes[index].sourcePos, fmt::format(
          "{}() {} must be a constant numeric scalar", function, argument));
    }
    const ValueNode& node = program->nodes[index];
    return node.type == ValueType::DOUBLE ? node.doubleValue : (double)node.intValue;
  }

  uint32_t constantDouble(double value, size_t pos) {
    ValueNode node;
    node.kind = ValueNodeKind::CONSTANT;
    node.type = ValueType::DOUBLE;
    node.doubleValue = value;
    node.sourcePos = pos;
    node.text = program->copyString(fmt::format("{}", value));
    return append(node);
  }

  uint32_t call(std::string_view name, size_t pos,
                std::initializer_list<uint32_t> children) {
    return makeFunction(name, pos, std::span<const uint32_t>(children.begin(), children.size()));
  }

  // Elasticsearch decay functions over a numeric, DATE, or geo_distance()
  // value, expanded into primitives:
  //   d = max(0, |value - origin| - offset)   (min over a multi-valued field)
  //   gauss:  exp(ln(decay) * (d / scale)^2)
  //   exp:    exp(ln(decay) * d / scale)
  //   linear: max(0, 1 - d / (scale / (1 - decay)))
  // A document without a value scores 1, as in Elasticsearch. exp() with one
  // argument is the exponential function.
  template <class Grammar>
  uint32_t parseDecayOrExp(Grammar& grammar, std::string_view name, size_t pos) {
    std::array<uint32_t, 5> args{};
    bool math = name == "exp";
    uint8_t count = grammar.parseArguments(
        name, pos, math ? 1 : 3, 5,
        [&](uint8_t argument) { args[argument] = grammar.parseExpression(); });
    if (math && count == 1) {
      std::array<uint32_t, VALUE_MAX_ARITY> unary{args[0]};
      return appendFunction(ValueFunctionRegistry::find("exp"), pos, unary, 1);
    }
    if (count < 3) {
      fail(pos, "exp() expects 1 argument (the exponential) or 3 to 5 "
                "(a decay: value, origin, scale, offset, decay)");
    }
    double scale = constantArg(args[2], name, "scale");
    double offset = count > 3 ? constantArg(args[3], name, "offset") : 0.0;
    double decay = count > 4 ? constantArg(args[4], name, "decay") : 0.5;
    if (!(scale > 0.0)) fail(pos, fmt::format("{}() scale must be positive", name));
    if (!(offset >= 0.0)) fail(pos, fmt::format("{}() offset must be non-negative", name));
    if (!(decay > 0.0 && decay < 1.0)) {
      fail(pos, fmt::format("{}() decay must be strictly between 0 and 1", name));
    }
    if (valueArray(program->nodes[args[1]].type)) {
      fail(pos, fmt::format("{}() origin must be a scalar", name));
    }

    uint32_t distance = call("abs", pos, {call("sub", pos, {args[0], args[1]})});
    if (valueArray(program->nodes[distance].type)) distance = call("min", pos, {distance});
    if (offset > 0.0) {
      distance = call("max", pos, {call("sub", pos, {distance, constantDouble(offset, pos)}),
                                   constantDouble(0.0, pos)});
    }
    uint32_t score;
    if (name == "linear") {
      uint32_t ratio = call("div", pos, {distance, constantDouble(scale / (1.0 - decay), pos)});
      score = call("max", pos, {call("sub", pos, {constantDouble(1.0, pos), ratio}),
                                constantDouble(0.0, pos)});
    } else {
      uint32_t ratio = call("div", pos, {distance, constantDouble(scale, pos)});
      if (name == "gauss") ratio = call("mul", pos, {ratio, ratio});
      score = call("exp", pos, {call("mul", pos, {constantDouble(std::log(decay), pos), ratio})});
    }
    return call("def", pos, {score, constantDouble(1.0, pos)});
  }
};

} // namespace luxir
