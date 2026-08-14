#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
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
#include "luxir/value/ValueExpr.h"
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
  Cursor cur{std::string_view{}};
  ValueProgram* program = nullptr;
  int localBudget = DEFAULT_NESTING_BUDGET;
  int* budget;

  struct DepthScope {
    ValueExprParser& parser;

    DepthScope(ValueExprParser& parser, size_t pos) : parser(parser) {
      if (*parser.budget <= 0) {
        parser.fail(pos, "expression nesting exceeds the supported depth");
      }
      --*parser.budget;
    }
    ~DepthScope() { ++*parser.budget; }
  };

public:
  ValueExprParser(const ValueExprOptions& opts, google::protobuf::Arena& arena)
      : opts(opts), arena(arena), budget(opts.nestingBudget ? opts.nestingBudget : &localBudget) {}

  ValueProgram* parse(std::string_view expression) {
    if (opts.schema == nullptr) throw std::runtime_error("ValueExpr requires a schema");
    cur = Cursor(expression);
    program = arenaCreate<ValueProgram>(arena, arena);
    cur.skipWs();
    if (cur.atEnd()) fail(0, "empty value expression");
    program->rootNode = parseValue();
    cur.skipWs();
    if (!cur.atEnd()) fail(cur.position(), "unexpected trailing input");
    program->constantScalar = findConstantScalar();
    return program;
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

  [[noreturn]] void fail(size_t pos, std::string_view message) const {
    size_t from = pos > 20 ? pos - 20 : 0;
    throw std::runtime_error(fmt::format(
        "value expression parse error at byte {}: {} (context: \"{}<HERE>{}\")",
        pos, message, cur.slice(from, pos), cur.slice(pos, pos + 20)));
  }

  uint32_t append(ValueNode node) {
    program->addNode(node);
    return (uint32_t)program->nodes.size() - 1;
  }

  uint32_t parseValue() {
    cur.skipWs();
    size_t pos = cur.position();
    char c = cur.peek();
    if (c == '$') return parseVariable();
    if (c == '"' || c == '\'') {
      fail(pos, "a quoted string is only valid as the argument of col(\"name\")");
    }
    if (c == '+' || c == '-' || c == '.' || value::lex::digit(c)) {
      return parseNumber();
    }
    std::string_view name = value::lex::scanIdentifier(cur);
    if (name.empty()) fail(pos, "expected a number, numeric column, variable, score, or function");
    if (cur.peek() == '(') return parseCall(name, pos);
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
    size_t pos = cur.position();
    std::string_view text = value::lex::scanNumber(cur);
    if (text.empty()) fail(pos, "invalid numeric literal");
    char next = cur.peek();
    if (!cur.atEnd() && cur.wsLen() == 0 && next != ',' && next != ')') {
      fail(pos, fmt::format("invalid numeric literal '{}'", cur.slice(pos, cur.position() + 1)));
    }

    ValueNode node;
    node.kind = ValueNodeKind::CONSTANT;
    node.sourcePos = pos;
    node.text = program->copyString(text);
    bool floating = text.find_first_of(".eE") != std::string_view::npos;
    if (!floating) {
      auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), node.intValue);
      if (ec != std::errc() || ptr != text.data() + text.size()) {
        fail(pos, fmt::format("int64 literal '{}' is out of range", text));
      }
      node.type = ValueType::INT64;
    } else {
      auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), node.doubleValue);
      if (ec != std::errc() || ptr != text.data() + text.size() ||
          !std::isfinite(node.doubleValue)) {
        fail(pos, fmt::format("double literal '{}' must be finite", text));
      }
      node.type = ValueType::DOUBLE;
    }
    return append(node);
  }

  uint32_t parseVariable() {
    size_t pos = cur.position();
    std::string_view name = value::lex::scanVariable(cur);
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
    }
    node.sourcePos = pos;
    node.text = program->copyString(name);
    return append(node);
  }

  uint32_t parseCall(std::string_view name, size_t pos) {
    DepthScope depth(*this, pos);
    cur.advance();
    if (name == "col") return parseExplicitColumn(pos);
    const ValueFunction* function = ValueFunctionRegistry::find(name);
    if (function == nullptr) fail(pos, fmt::format("unknown value function '{}()'", name));

    std::array<uint32_t, 2> args{};
    uint8_t count = 0;
    cur.skipWs();
    if (!cur.consume(')')) {
      for (;;) {
        if (count == args.size()) {
          fail(cur.position(), fmt::format("{}() accepts at most {} arguments", name,
                                           function->maxArity));
        }
        args[count++] = parseValue();
        auto separator = value::lex::consumeListSeparator(cur);
        if (separator == value::lex::ListSeparator::CLOSE) break;
        if (separator != value::lex::ListSeparator::COMMA) {
          fail(cur.position(), fmt::format("expected ',' or ')' in {}(...)", name));
        }
        cur.skipWs();
        if (cur.peek() == ')') fail(cur.position(), "trailing comma is not allowed");
      }
    }
    if (count < function->minArity || count > function->maxArity) {
      if (function->minArity == function->maxArity) {
        fail(pos, fmt::format("{}() expects {} argument{}", name, function->minArity,
                              function->minArity == 1 ? "" : "s"));
      }
      fail(pos, fmt::format("{}() expects {} to {} arguments", name,
                            function->minArity, function->maxArity));
    }
    std::array<ValueType, 2> types{};
    for (uint8_t i = 0; i < count; i++) types[i] = program->nodes[args[i]].type;

    ValueNode node;
    node.kind = ValueNodeKind::FUNCTION;
    node.opcode = function->opcode;
    node.function = function;
    node.children = args;
    node.childCount = count;
    node.sourcePos = pos;
    node.text = function->name;
    try {
      node.type = function->resolveType(std::span<const ValueType>(types.data(), count));
    } catch (const std::exception& error) {
      fail(pos, fmt::format("{}(): {}", name, error.what()));
    }
    return append(node);
  }

  uint32_t parseExplicitColumn(size_t pos) {
    cur.skipWs();
    if (cur.peek() != '"' && cur.peek() != '\'') {
      fail(cur.position(), "col() expects one quoted field name, for example col(\"price_i\")");
    }
    std::string_view name = value::lex::scanQuoted(cur, program->resource,
        [&](size_t at, std::string_view message) { fail(at, message); });
    cur.skipWs();
    if (!cur.consume(')')) fail(cur.position(), "col() expects exactly one quoted field name");
    return parseColumn(name, pos);
  }
};

} // namespace luxir
