#pragma once

#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/Schema.h"
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
};

class ValueExprParser {
  static constexpr int DEFAULT_NESTING_BUDGET = 128;

  const ValueExprOptions& opts;
  google::protobuf::Arena& arena;
  Cursor* cur = nullptr;
  ValueProgram* program = nullptr;
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
    cur->skipWs();
    if (cur->atEnd()) fail(cur->position(), "empty value expression");
    ValueExprGrammar grammar(*this, *cur, *budget);
    program->rootNode = grammar.parseExpression();
    program->constantScalar = findConstantScalar();
    return program;
  }

  uint32_t makeFunction(std::string_view name, size_t pos,
                        std::span<const uint32_t> args) {
    const ValueFunction* function = ValueFunctionRegistry::find(name);
    assert(function != nullptr);
    std::array<uint32_t, 2> children{};
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
  std::optional<ValueResult> findConstantScalar() const {
    std::vector<std::optional<ValueBounds>> bounds(program->nodes.size());
    for (uint32_t index = 0; index < program->nodes.size(); index++) {
      const ValueNode& node = program->nodes[index];
      if (node.kind == ValueNodeKind::CONSTANT
          || node.kind == ValueNodeKind::VARIABLE) {
        if (node.type == ValueType::INT64) {
          bounds[index] = ValueBounds::integer(node.intValue, node.intValue);
        } else if (node.type == ValueType::DOUBLE) {
          bounds[index] =
              ValueBounds::floating(node.doubleValue, node.doubleValue);
        } else if (node.arraySize != 0) {
          if (node.type == ValueType::INT64_ARRAY) {
            auto begin = program->intArrays.begin() + node.arrayOffset;
            auto extrema =
                std::minmax_element(begin, begin + node.arraySize);
            ValueBounds value =
                ValueBounds::integer(*extrema.first, *extrema.second);
            value.type = node.type;
            bounds[index] = value;
          } else {
            auto begin = program->doubleArrays.begin() + node.arrayOffset;
            auto extrema =
                std::minmax_element(begin, begin + node.arraySize);
            ValueBounds value =
                ValueBounds::floating(*extrema.first, *extrema.second);
            value.type = node.type;
            bounds[index] = value;
          }
        }
      } else if (node.kind == ValueNodeKind::FUNCTION) {
        std::array<ValueBounds, 2> children;
        bool independent = true;
        for (uint8_t child = 0; child < node.childCount; child++) {
          const auto& childBounds = bounds[node.children[child]];
          if (!childBounds.has_value()) {
            independent = false;
            break;
          }
          children[child] = *childBounds;
        }
        if (independent) {
          ValueBounds value = node.function->boundsPropagate(
              node, std::span<const ValueBounds>(
                        children.data(), node.childCount));
          if (value.certainty != BoundsCertainty::INVALID) {
            bounds[index] = value;
          }
        }
      }
    }

    const auto& rootBounds = bounds[program->rootNode];
    ValueType rootType = program->root().type;
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
        && rootBounds->doubleMin == rootBounds->doubleMax) {
      return ValueResult::floating(rootBounds->doubleMin);
    }
    return std::nullopt;
  }

  uint32_t append(ValueNode node) {
    program->addNode(node);
    return (uint32_t)program->nodes.size() - 1;
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
    FieldType* field = opts.schema->getFieldTypePtr(name);
    if (field == nullptr) fail(pos, fmt::format("unknown field '{}'", name));
    bool numeric = field->type() == FieldType::INT || field->type() == FieldType::DATE ||
                   field->type() == FieldType::FLOAT || field->type() == FieldType::DOUBLE;
    bool stringSortable = field->type() == FieldType::ID || field->type() == FieldType::STRING ||
                          field->type() == FieldType::TEXT;
    if (!numeric && !stringSortable) fail(pos, fmt::format("field '{}' is not sortable", name));
    if (numeric && !field->hasColumn()) {
      fail(pos, fmt::format("field '{}' has no column values", name));
    }
    if (stringSortable && !field->hasColumn() && !field->indexed()) {
      fail(pos, fmt::format("field '{}' has no sortable values", name));
    }

    ValueNode node;
    node.kind = ValueNodeKind::COLUMN;
    node.columnType = field->type();
    node.columnMultiValued = field->multiValued();
    if (!numeric) {
      node.type = ValueType::COLUMN_ONLY;
    } else {
      bool floating = field->type() == FieldType::FLOAT || field->type() == FieldType::DOUBLE;
      node.type = field->multiValued()
          ? (floating ? ValueType::DOUBLE_ARRAY : ValueType::INT64_ARRAY)
          : (floating ? ValueType::DOUBLE : ValueType::INT64);
      node.nature = field->type() == FieldType::DATE
          ? ValueNature::DATE : ValueNature::NUMBER;
    }
    node.sourcePos = pos;
    node.text = program->copyString(name);
    return append(node);
  }

  template <class Grammar>
  uint32_t parseCall(Grammar& grammar, std::string_view name, size_t pos) {
    if (name == "col") return parseExplicitColumn(grammar, pos);
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

    std::array<uint32_t, 2> args{};
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
                          std::array<uint32_t, 2> args, uint8_t count) {
    assert(function != nullptr);
    std::array<ResolvedValue, 2> resolvedArgs{};
    for (uint8_t i = 0; i < count; i++) {
      const ValueNode& arg = program->nodes[args[i]];
      resolvedArgs[i] = {arg.type, arg.nature};
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
      ResolvedValue resolved = function->resolve(
          std::span<const ResolvedValue>(resolvedArgs.data(), count));
      node.type = resolved.type;
      node.nature = resolved.nature;
    } catch (const std::exception& error) {
      fail(pos, fmt::format("{}(): {}", function->name, error.what()));
    }
    return append(node);
  }
};

} // namespace luxir
