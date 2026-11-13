// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
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

  // A let variable or a fold accumulator or counter, visible while the body that
  // declares it is parsed.
  struct Binding {
    enum class Kind : uint8_t { LET, ACCUMULATOR, COUNTER };
    std::string_view name;
    uint32_t slot = 0;
    ValueType type = ValueType::INT64;
    ValueNature nature = ValueNature::NUMBER;
    Kind kind = Kind::LET;
    uint32_t init = 0;  // LET: the node holding the value
    int64_t steps = 0;  // COUNTER: the number of steps of its loop
  };

  const ValueExprOptions& opts;
  google::protobuf::Arena& arena;
  Cursor* cur = nullptr;
  ValueProgram* program = nullptr;
  ValueConstantAnalysis constants;
  std::vector<Binding> bindings;
  // The product of the step counts of the loops around the body being parsed.
  int64_t loopWork = 1;
  int localBudget = DEFAULT_NESTING_BUDGET;
  int* budget;

public:
  using Node = uint32_t;

  // fold() runs a fixed number of steps: at most MAX_LOOP_STEPS for one loop and
  // MAX_LOOP_WORK in total for loops nested in each other, so the cost of a
  // document's evaluation is bounded by the size of the expression.
  static constexpr int64_t MAX_LOOP_STEPS = 256;
  static constexpr int64_t MAX_LOOP_WORK = 4096;
  static constexpr uint32_t MAX_LOCALS = 64;

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
    bindings.clear();
    loopWork = 1;
    cur->skipWs();
    if (cur->atEnd()) fail(cur->position(), "empty value expression");
    ValueExprGrammar grammar(*this, *cur, *budget);
    program->rootNode = grammar.parseExpression();
    if (program->root().nature == ValueNature::GEO_POINT) {
      fail(program->root().sourcePos, "GEO_POINT is not numeric; use geo_distance(), or geo_lat() and geo_lon()");
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
    if (name == "score" || name == "_score" || (opts.sortIntrinsics && name == "_score_")) {
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
    if (const Binding* binding = findBinding(name)) return appendLocal(*binding, pos);
    auto failAt = [&](size_t at, std::string_view message) { fail(at, message); };
    if (name == "params") {
      std::string_view variable =
          value::lex::scanParamsAccessor(*cur, program->resource, failAt);
      if (!variable.empty()) return parseVariableNamed(variable, pos);
    }
    if (name == "doc" && cur->peek() == '[') {
      std::string_view field = value::lex::scanBracketedName(*cur, program->resource, failAt);
      return parseAccessors(parseColumn(field, pos));
    }
    return parseAccessors(parseColumn(name, pos));
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
    return parseVariableNamed(name, pos);
  }

  // $name, params.name and params['name'] are the same variable.
  uint32_t parseVariableNamed(std::string_view name, size_t pos) {
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
    try {
      field->requireCapability(FieldType::VALUE_EXPR, "value expression", name);
    } catch (const RequestError& e) {
      fail(pos, e.what());
    }
    bool numeric = FieldType::isNumericColumn(field->type());
    bool geo = field->type() == FieldType::GEO_POINT;
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
      node.nature = field->valueNature();
    }
    node.sourcePos = pos;
    node.text = program->copyString(target.physicalName);
    return append(node);
  }

  template <class Grammar>
  uint32_t parseCall(Grammar& grammar, std::string_view name, size_t pos) {
    if (name == "col") return parseExplicitColumn(grammar, pos);
    if (name == "let") return parseLet(grammar, pos);
    if (name == "fold") return parseFold(grammar, pos);
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
    return parseAccessors(parseColumn(name, pos));
  }

  void rewriteDecimalComparison(const ValueFunction*& function,
      std::array<uint32_t, VALUE_MAX_ARITY>& args, uint8_t count,
      std::array<ResolvedValue, VALUE_MAX_ARITY>& resolved) {
    if (count != 2) return;
    auto op = function->opcode;
    using Op = ValueOpcode;
    if (op != Op::LT && op != Op::LE && op != Op::GT && op != Op::GE && op != Op::EQ && op != Op::NE) return;
    size_t field = valueDecimal(resolved[0].nature) ? 0 : 1;
    size_t literal = 1 - field;
    if (!valueDecimal(resolved[field].nature) || resolved[field].type != ValueType::INT64) return;
    auto constant = constants.scalar(args[literal]);
    if (!constant || resolved[literal].nature != ValueNature::NUMBER) return;
    const auto* source = &program->nodes[args[literal]];
    bool negate = false;
    while (source->kind == ValueNodeKind::FUNCTION && source->opcode == Op::NEG) {
      negate = !negate;
      source = &program->nodes[source->children[0]];
    }
    std::string signedLiteral;
    api::Val literalValue;
    if (source->kind == ValueNodeKind::CONSTANT && !source->text.empty()
        && (value::lex::digit(source->text.front()) || source->text.front() == '.'
            || source->text.front() == '-' || source->text.front() == '+')) {
      auto text = source->text;
      if (negate) {
        if (text.front() == '-') text.remove_prefix(1);
        else {
          if (text.front() == '+') text.remove_prefix(1);
          signedLiteral = "-" + std::string(text);
          text = signedLiteral;
        }
      }
      literalValue.kind = text;
    } else if (constant->type == ValueType::INT64) literalValue.kind = constant->intValue;
    else literalValue.kind = constant->doubleValue;
    auto comparison = ValueFunctionRegistry::decimalComparison(function, valueDecimalScale(resolved[field].nature),
        literalValue, field == 1);
    if (!comparison) return;
    function = comparison->function;
    ValueNode bound;
    bound.nature = resolved[field].nature;
    bound.intValue = comparison->bound;
    uint32_t fieldNode = args[field];
    args[0] = fieldNode;
    args[1] = append(bound);
    resolved[0] = {program->nodes[fieldNode].type, program->nodes[fieldNode].nature};
    resolved[1] = {ValueType::INT64, bound.nature};
  }

  uint32_t appendFunction(const ValueFunction* function, size_t pos,
                          std::array<uint32_t, VALUE_MAX_ARITY> args, uint8_t count) {
    assert(function != nullptr);
    std::array<ResolvedValue, VALUE_MAX_ARITY> resolvedArgs{};
    for (uint8_t i = 0; i < count; i++) {
      const ValueNode& arg = program->nodes[args[i]];
      resolvedArgs[i] = {arg.type, arg.nature};
    }
    rewriteDecimalComparison(function, args, count, resolvedArgs);
    auto natureConversions = function->natureConversions({resolvedArgs.data(), count});
    for (uint8_t i = 0; i < count; i++) {
      if (!natureConversions[i]) continue;
      size_t start = program->nodes.size();
      args[i] = program->numericNode(args[i]);
      for (size_t n = start; n < program->nodes.size(); n++) constants.append(program->nodes[n]);
      resolvedArgs[i] = {program->nodes[args[i]].type, program->nodes[args[i]].nature};
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
      if (function->prepare != nullptr) {
        std::array<const ValueNode*, VALUE_MAX_ARITY> operands{};
        for (uint8_t i = 0; i < count; i++) operands[i] = &program->nodes[args[i]];
        function->prepare(node, {operands.data(), count});
      }
    } catch (const std::exception& error) {
      fail(pos, fmt::format("{}(): {}", function->name, error.what()));
    }
    return append(node);
  }

  // ---- accessors on a column: doc['f'].value, loc.lat, ts.date.year ----------

  uint32_t parseAccessors(uint32_t node) {
    for (;;) {
      if (cur->peek() != '.' || !value::lex::identifierStart(cur->peekAt(1))) return node;
      size_t pos = cur->position();
      cur->advance();
      std::string_view name = value::lex::scanIdentifier(*cur);
      if (cur->peek() == '(' && cur->peekAt(1) == ')') cur->advance(2);
      node = applyAccessor(node, name, pos);
    }
  }

  uint32_t call(std::string_view function, size_t pos, std::initializer_list<uint32_t> args) {
    std::array<uint32_t, VALUE_MAX_ARITY> nodes{};
    std::copy(args.begin(), args.end(), nodes.begin());
    return makeFunction(function, pos, std::span<const uint32_t>(nodes.data(), args.size()));
  }

  uint32_t appendInteger(int64_t value, size_t pos) {
    ValueNode node;
    node.kind = ValueNodeKind::CONSTANT;
    node.type = ValueType::INT64;
    node.intValue = value;
    node.sourcePos = pos;
    node.text = program->copyString(std::to_string(value));
    return append(node);
  }

  // The Lucene expressions and Painless spellings of column access. A getter
  // prefix is ignored (getYear() is year), and .date is a no-op step, so
  // doc['ts'].date.getYear(), doc['ts'].date.year and ts.year are the same.
  uint32_t applyAccessor(uint32_t node, std::string_view spelling, size_t pos) {
    std::string key(spelling);
    if (key.size() > 3 && key.starts_with("get") && key[3] >= 'A' && key[3] <= 'Z') {
      key.erase(0, 3);
      key[0] = (char)(key[0] - 'A' + 'a');
    }
    bool array = valueArray(program->nodes[node].type);
    if (key == "value" || key == "date") return node;
    if (key == "lat") return call("geo_lat", pos, {node});
    if (key == "lon") return call("geo_lon", pos, {node});
    if (key == "size" || key == "length") return call("count", pos, {node});
    if (key == "empty") {
      uint32_t count = call("count", pos, {node});
      return call("eq", pos, {count, appendInteger(0, pos)});
    }
    if (key == "min" || key == "max" || key == "avg" || key == "sum") {
      return array ? call(key, pos, {node}) : node;
    }
    static constexpr std::pair<std::string_view, std::string_view> DATE_PARTS[] = {
        {"year", "year"}, {"monthOfYear", "month"}, {"month", "month"},
        {"dayOfMonth", "day_of_month"}, {"dayOfWeek", "day_of_week"},
        {"dayOfYear", "day_of_year"}, {"hourOfDay", "hour"}, {"hour", "hour"},
        {"minuteOfHour", "minute"}, {"minute", "minute"}, {"secondOfMinute", "second"},
        {"second", "second"}, {"millisOfSecond", "millis"}, {"millis", "millis"},
        {"millisOfDay", "millis_of_day"}, {"weekOfWeekyear", "week_of_year"},
        {"weekOfYear", "week_of_year"}, {"quarter", "quarter"},
    };
    for (const auto& [accessor, function] : DATE_PARTS) {
      if (key == accessor) return call(function, pos, {node});
    }
    fail(pos, fmt::format(
        "unknown accessor '.{}'; use .value, .lat, .lon, .size(), .length, .empty, .min, .max, "
        ".avg, .sum, or a date part such as .date.year", spelling));
  }

  // ---- let and fold ---------------------------------------------------------

  const Binding* findBinding(std::string_view name) const {
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
      if (it->name == name) return &*it;
    }
    return nullptr;
  }

  uint32_t appendLocal(const Binding& binding, size_t pos) {
    ValueNode node;
    node.kind = ValueNodeKind::LOCAL;
    node.type = binding.type;
    node.nature = binding.nature;
    node.slot = binding.slot;
    node.sourcePos = pos;
    node.text = program->copyString(binding.name);
    // propagateBounds reads these: a let variable has its value's bounds and a
    // counter the range of its loop.
    if (binding.kind == Binding::Kind::LET) {
      node.childCount = 1;
      node.children[0] = binding.init;
    } else if (binding.kind == Binding::Kind::COUNTER) {
      node.arraySize = 1;
      node.intValue = binding.steps;
    }
    return append(node);
  }

  std::string_view parseBindingName() {
    cur->skipWs();
    size_t pos = cur->position();
    std::string_view name = value::lex::scanIdentifier(*cur);
    if (name.empty()) fail(pos, "expected a variable name");
    for (std::string_view reserved : {"score", "_score", "_score_", "_docid_", "doc", "params", "col"}) {
      if (name == reserved) fail(pos, fmt::format("'{}' cannot be used as a variable name", name));
    }
    if (findBinding(name) != nullptr) fail(pos, fmt::format("variable '{}' is already defined", name));
    return program->copyString(name);
  }

  uint32_t allocateSlot(size_t pos) {
    if (program->localCount >= MAX_LOCALS) {
      fail(pos, fmt::format("an expression may declare at most {} variables", MAX_LOCALS));
    }
    return program->localCount++;
  }

  // A decimal or boolean value is a number to the rest of the expression.
  uint32_t numericScalar(uint32_t node, std::string_view what) {
    const ValueNode& value = program->nodes[node];
    if (valueArray(value.type)) {
      fail(value.sourcePos, fmt::format(
          "{} must be a scalar; reduce an array first, for example with min() or max()", what));
    }
    if (value.type == ValueType::COLUMN_ONLY || value.nature == ValueNature::IP) {
      fail(value.sourcePos, fmt::format("{} must be numeric", what));
    }
    if (valueDecimal(value.nature) || value.nature == ValueNature::BOOLEAN) {
      size_t start = program->nodes.size();
      node = program->numericNode(node);
      for (size_t n = start; n < program->nodes.size(); n++) constants.append(program->nodes[n]);
    }
    return node;
  }

  template <class Grammar>
  uint32_t parseLet(Grammar& grammar, size_t pos) {
    std::string_view name;
    uint32_t value = 0;
    uint32_t body = 0;
    uint32_t slot = 0;
    grammar.parseArguments("let", pos, 3, 3, [&](uint8_t argument) {
      if (argument == 0) {
        name = parseBindingName();
      } else if (argument == 1) {
        value = numericScalar(grammar.parseExpression(), "the value of let()");
      } else {
        slot = allocateSlot(pos);
        const ValueNode& bound = program->nodes[value];
        bindings.push_back({name, slot, bound.type, bound.nature, Binding::Kind::LET, value, 0});
        body = numericScalar(grammar.parseExpression(), "the body of let()");
        bindings.pop_back();
      }
    });
    ValueNode node;
    node.kind = ValueNodeKind::FUNCTION;
    node.function = ValueFunctionRegistry::find("let");
    node.children[0] = value;
    node.children[1] = body;
    node.childCount = 2;
    node.type = program->nodes[body].type;
    node.nature = program->nodes[body].nature;
    node.slot = slot;
    node.sourcePos = pos;
    node.text = node.function->name;
    return append(node);
  }

  template <class Grammar>
  uint32_t parseFold(Grammar& grammar, size_t pos) {
    std::string_view accumulator;
    std::string_view counter;
    uint32_t init = 0;
    uint32_t count = 0;
    uint32_t body = 0;
    uint32_t accumulatorSlot = 0;
    uint32_t counterSlot = 0;
    ValueType type = ValueType::INT64;
    grammar.parseArguments("fold", pos, 5, 5, [&](uint8_t argument) {
      switch (argument) {
        case 0:
          accumulator = parseBindingName();
          break;
        case 1: {
          init = numericScalar(grammar.parseExpression(), "the initial value of fold()");
          const ValueNode& value = program->nodes[init];
          if (value.nature != ValueNature::NUMBER) {
            fail(value.sourcePos, "the initial value of fold() must be a number");
          }
          type = value.type;
          break;
        }
        case 2:
          counter = parseBindingName();
          if (counter == accumulator) {
            fail(cur->position(), "the loop counter and the accumulator need different names");
          }
          break;
        case 3: {
          size_t at = cur->position();
          count = grammar.parseExpression();
          if (!foldConstant(count) || program->nodes[count].type != ValueType::INT64) {
            fail(at, "the number of steps of fold() must be an integer constant");
          }
          break;
        }
        default: {
          int64_t steps = program->nodes[count].intValue;
          if (steps < 0 || steps > MAX_LOOP_STEPS) {
            fail(pos, fmt::format("fold() runs from 0 to {} steps, not {}", MAX_LOOP_STEPS, steps));
          }
          int64_t outer = loopWork;
          if (steps > 0 && outer > MAX_LOOP_WORK / steps) {
            fail(pos, fmt::format("nested fold() loops may run at most {} steps in total",
                                  MAX_LOOP_WORK));
          }
          accumulatorSlot = allocateSlot(pos);
          counterSlot = allocateSlot(pos);
          bindings.push_back({accumulator, accumulatorSlot, type, ValueNature::NUMBER,
                              Binding::Kind::ACCUMULATOR, 0, 0});
          bindings.push_back({counter, counterSlot, ValueType::INT64, ValueNature::NUMBER,
                              Binding::Kind::COUNTER, 0, steps});
          loopWork = outer * std::max<int64_t>(steps, 1);
          body = numericScalar(grammar.parseExpression(), "the body of fold()");
          loopWork = outer;
          bindings.pop_back();
          bindings.pop_back();
          const ValueNode& result = program->nodes[body];
          if (result.nature != ValueNature::NUMBER) {
            fail(result.sourcePos, "the body of fold() must be a number");
          }
          if (type == ValueType::INT64 && result.type == ValueType::DOUBLE) {
            fail(result.sourcePos,
                 "the body of fold() is a double but the accumulator starts as an integer; "
                 "start it from a double, for example 0.0");
          }
        }
      }
    });
    ValueNode node;
    node.kind = ValueNodeKind::FUNCTION;
    node.function = ValueFunctionRegistry::find("fold");
    node.children[0] = init;
    node.children[1] = body;
    node.children[2] = count;
    node.childCount = 3;
    node.type = type;
    node.nature = ValueNature::NUMBER;
    node.slot = accumulatorSlot;
    node.counterSlot = counterSlot;
    node.sourcePos = pos;
    node.text = node.function->name;
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

};

} // namespace luxir
