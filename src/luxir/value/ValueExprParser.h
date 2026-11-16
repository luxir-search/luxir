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
    enum class State : uint8_t { DEPENDENT, ARRAY, VALUE, FAILED };
    struct Constant {
      ValueResult value;
      State state = State::DEPENDENT;
      bool independent() const { return state != State::DEPENDENT; }
    };
    std::vector<Constant> constants;
    std::optional<BoundValueProgram> evaluator;

    std::optional<ValueResult> fold(ValueProgram& program, uint32_t index) {
      ValueNode& node = program.nodes[index];
      ValueResult result;
      std::vector<int64_t> integers;
      std::vector<double> doubles;
      try {
        result = evaluator->evalNode(index, 0, 0);
        if (result.valid && valueArray(result.type)) {
          for (int64_t i = 0; i < result.array.size; i++) {
            auto element = evaluator->evalArrayElement(result.array, i);
            // Literal arrays have no missing-element representation.
            if (!element.valid) return std::nullopt;
            if (element.type == ValueType::INT64) integers.push_back(element.intValue);
            else doubles.push_back(element.doubleValue);
          }
        }
      } catch (const ValueEvaluationError&) {
        // Keep errors lazy: an unused result or untaken branch must not fail.
        return std::nullopt;
      }
      if (node.kind == ValueNodeKind::FUNCTION) {
        ValueNode literal;
        literal.kind = result.valid ? ValueNodeKind::CONSTANT : ValueNodeKind::MISSING;
        literal.type = node.type;
        literal.nature = node.nature;
        literal.sourcePos = node.sourcePos;
        literal.text = node.text;
        if (node.opcode == ValueOpcode::NEG) {
          const auto& input = program.nodes[node.children[0]];
          auto text = input.text;
          if (input.kind == ValueNodeKind::CONSTANT && !text.empty()
              && (value::lex::digit(text.front()) || text.front() == '.'
                  || text.front() == '-' || text.front() == '+')) {
            std::string signedText;
            if (text.front() == '-') signedText = text.substr(1);
            else {
              if (text.front() == '+') text.remove_prefix(1);
              signedText = "-";
              signedText += text;
            }
            literal.text = program.copyString(signedText);
          }
        }
        literal.intValue = result.intValue;
        literal.doubleValue = result.doubleValue;
        if (result.valid && valueArray(result.type)) {
          literal.arraySize = (uint32_t)result.array.size;
          if (node.type == ValueType::INT64_ARRAY) {
            literal.arrayOffset = (uint32_t)program.intArrays.size();
            program.intArrays.insert(program.intArrays.end(), integers.begin(), integers.end());
          } else {
            literal.arrayOffset = (uint32_t)program.doubleArrays.size();
            program.doubleArrays.insert(program.doubleArrays.end(), doubles.begin(), doubles.end());
          }
        }
        node = literal;
      }
      return result;
    }

  public:
    void reset(ValueProgram& program) {
      constants.clear();
      evaluator.emplace(program);
    }

    void append(ValueProgram& program, uint32_t index) {
      const ValueNode& node = program.nodes[index];
      bool pure = node.kind == ValueNodeKind::CONSTANT || node.kind == ValueNodeKind::VARIABLE
          || node.kind == ValueNodeKind::MISSING;
      uint32_t required = 0;
      if (node.kind == ValueNodeKind::FUNCTION) {
        pure = node.function->pure;
        for (uint8_t child = 0; child < node.childCount; child++) {
          pure = pure && constants[node.children[child]].independent();
          if (!node.function->operands[child].lazy) required |= 1u << child;
        }
        if (node.function->pure && node.childCount
            && constants[node.children[0]].state == State::VALUE && !valueArray(node.type)) {
          const auto& first = constants[node.children[0]].value;
          bool truth = first.type == ValueType::DOUBLE ? first.doubleValue != 0 : first.intValue != 0;
          switch (node.opcode) {
            case ValueOpcode::IF:
              pure = !first.valid || constants[node.children[truth ? 1 : 2]].independent();
              required = first.valid ? 1u | (1u << (truth ? 1 : 2)) : 1u;
              break;
            case ValueOpcode::DEF:
              pure = first.valid || constants[node.children[1]].independent();
              required = first.valid ? 1u : 3u;
              break;
            case ValueOpcode::AND:
              pure = !first.valid || !truth || constants[node.children[1]].independent();
              required = !first.valid || !truth ? 1u : 3u;
              break;
            case ValueOpcode::OR:
              pure = !first.valid || truth || constants[node.children[1]].independent();
              required = !first.valid || truth ? 1u : 3u;
              break;
            default: break;
          }
        }
      }
      Constant result;
      if (pure) {
        // Failed dependencies remain unfolded, but are never retried through
        // each parent of a DAG. Only operands that will be selected contribute.
        bool failed = false;
        for (uint8_t child = 0; child < node.childCount; child++) {
          failed = failed || ((required & (1u << child))
              && constants[node.children[child]].state == State::FAILED);
        }
        if (failed) result.state = State::FAILED;
        else if (valueArray(node.type)) result.state = State::ARRAY;
        else if (node.type != ValueType::COLUMN_ONLY) {
          auto value = fold(program, index);
          result.state = value ? State::VALUE : State::FAILED;
          if (value) result.value = *value;
        }
      }
      constants.push_back(result);
    }

    void finishArrays(ValueProgram& program) {
      // Materialize only surviving pure array roots. Folding each intermediate
      // array at append would retain depth * array-size data in the arena.
      program.walkNodes(program.rootNode, [&](uint32_t index) {
        const ValueNode& node = program.nodes[index];
        if (constants[index].independent() && valueArray(node.type) && node.kind == ValueNodeKind::FUNCTION) {
          if (constants[index].state != State::FAILED) (void)fold(program, index);
          return 0u;
        }
        return (1u << node.childCount) - 1;
      }, [](uint32_t) {});
    }

    std::optional<ValueResult> scalar(uint32_t index) const {
      const auto& value = constants[index];
      return value.state == State::VALUE && value.value.valid
          ? std::optional<ValueResult>(value.value) : std::nullopt;
    }
  };

  static constexpr int DEFAULT_NESTING_BUDGET = 128;

  struct Binding {
    std::string_view name;
    uint32_t node;
  };

  const ValueExprOptions& opts;
  google::protobuf::Arena& arena;
  Cursor* cur = nullptr;
  ValueProgram* program = nullptr;
  ValueConstantAnalysis constants;
  std::vector<Binding> bindings;
  std::vector<int> depths;
  int depthLimit = 0;
  int localBudget = DEFAULT_NESTING_BUDGET;
  int* budget;

public:
  using Node = uint32_t;

  ValueExprParser(const ValueExprOptions& opts, google::protobuf::Arena& arena)
      : opts(opts), arena(arena), budget(opts.nestingBudget ? opts.nestingBudget : &localBudget) {}

  ValueProgram* parse(std::string_view expression, NaturePolicy resultNatures = NaturePolicy::RAW) {
    Cursor cursor(expression);
    ValueProgram* result = parsePartial(cursor, resultNatures);
    cursor.skipWs();
    if (!cursor.atEnd()) fail(cursor.position(), "unexpected trailing input");
    cur = nullptr;
    return result;
  }

  // Parse one expression from the cursor's current absolute position and stop
  // before a caller-owned comma or close parenthesis. The caller and this
  // parser therefore share source positions without substring translation.
  // Result coercions use the same node construction and depth accounting.
  ValueProgram* parsePartial(Cursor& cursor, NaturePolicy resultNatures = NaturePolicy::RAW) {
    if (opts.schema == nullptr) throw std::runtime_error("ValueExpr requires a schema");
    cur = &cursor;
    program = arenaCreate<ValueProgram>(arena, arena);
    constants.reset(*program);
    bindings.clear();
    depths.clear();
    depthLimit = *budget;
    cur->skipWs();
    if (cur->atEnd()) fail(cur->position(), "empty value expression");
    ValueExprGrammar grammar(*this, *cur, *budget);
    program->rootNode = grammar.parseExpression();
    ResolvedValue result{program->root().type, program->root().nature};
    if (convertValueNature(resultNatures, {&result, 1}, 0)) {
      program->rootNode = numericNode(program->rootNode);
    }
    if (program->root().nature == ValueNature::GEO_POINT) {
      fail(program->root().sourcePos, "GEO_POINT is not numeric; use geo_distance(), or geo_lat() and geo_lon()");
    }
    constants.finishArrays(*program);
    program->constantScalar = constants.scalar(program->rootNode);
    program->retainRoot();
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
    analyzeNode(index);
    return index;
  }

  void analyzeNode(uint32_t index) {
    constants.append(*program, index);
    const ValueNode& node = program->nodes[index];
    int depth = 0;
    for (uint8_t i = 0; i < node.childCount; i++) {
      depth = std::max(depth, 1 + depths[node.children[i]]);
    }
    if (depth > depthLimit) fail(node.sourcePos, "expression nesting exceeds the supported depth");
    depths.push_back(depth);

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
    if (const Binding* binding = findBinding(name)) return binding->node;
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
    return parseVariableNamed(name, pos);
  }

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
    // A dot after a nested path continues the field name; after any other
    // field it introduces a type component.
    if (cur->peek() == '.') {
      std::string_view view = opts.fieldResolver ? opts.fieldResolver->path() : std::string_view{};
      if (opts.schema->isNestedPath(view.empty() ? std::string(name) : Schema::qualify(view, name))) {
        while (cur->consume('.')) {
          if (value::lex::scanIdentifier(*cur).empty()) fail(pos, "expected a field component after '.'");
        }
        name = cur->slice(pos, cur->position());
      }
    }
    ResolvedFieldHandle target;
    try {
      target = opts.fieldResolver ? opts.fieldResolver->resolveLocal(name, OpClass::VALUE, opts.opName)
          : FieldResolver(*opts.schema).resolveLocal(name, OpClass::VALUE, opts.opName);
    } catch (const RequestError& e) {
      fail(pos, e.what());
    }
    FieldType* field = target.fieldType;
    const ValueFunction* componentFunction = nullptr;
    size_t componentPos = pos;
    if (cur->consume('.')) {
      componentPos = cur->position();
      std::string_view component = value::lex::scanIdentifier(*cur);
      // Resolve real child fields here before type components when the schema
      // supports nested documents. Leaf capability checks follow resolution.
      auto kind = field->type() == FieldType::DATE ? ValueFunction::Kind::DATE
          : field->type() == FieldType::GEO_POINT ? ValueFunction::Kind::GEO_POINT
          : ValueFunction::Kind::NUMERIC;
      if (component.empty()) fail(componentPos, "a component must immediately follow the dot");
      std::string valid = ValueFunctionRegistry::componentNames(kind);
      if (valid.empty()) fail(componentPos, fmt::format(
          "field '{}' of type {} has no components", name, FieldType::typeName(field->type())));
      componentFunction = ValueFunctionRegistry::findComponent(kind, component);
      if (componentFunction == nullptr) fail(componentPos, fmt::format(
          "unknown component '{}' for field '{}' of type {}; valid components: {}",
          component, name, FieldType::typeName(field->type()), valid));
      if (cur->peek() == '.') fail(cur->position(), fmt::format(
          "only one component may follow field '{}'", name));
      if (cur->peek() == '(') fail(cur->position(), "field components do not take parentheses");
    }
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
    uint32_t index = append(node);
    if (componentFunction == nullptr) return index;
    std::array<uint32_t, VALUE_MAX_ARITY> args{index};
    return appendFunction(componentFunction, componentPos, args, 1);
  }

  template <class Grammar>
  uint32_t parseCall(Grammar& grammar, std::string_view name, size_t pos) {
    if (name == "col") return parseExplicitColumn(grammar, pos);
    if (name == "let") return parseLet(grammar, pos);
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
    auto numericText = source->text;
    if (numericText.starts_with('-') || numericText.starts_with('+')) numericText.remove_prefix(1);
    bool hex = numericText.starts_with("0x") || numericText.starts_with("0X");
    if (source->kind == ValueNodeKind::CONSTANT && !source->text.empty() && !hex
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
      args[i] = numericNode(args[i]);
      resolvedArgs[i] = {program->nodes[args[i]].type, program->nodes[args[i]].nature};
    }
    for (uint8_t i = 0; i < count; i++) {
      if (function->operands[i].constant && !constantOperand(args[i])) {
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

  // ---- let ---------------------------------------------------------

  const Binding* findBinding(std::string_view name) const {
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
      if (it->name == name) return &*it;
    }
    return nullptr;
  }

  std::string_view parseBindingName() {
    cur->skipWs();
    size_t pos = cur->position();
    std::string_view name = value::lex::scanIdentifier(*cur);
    if (name.empty()) fail(pos, "expected a variable name");
    for (std::string_view reserved : {"score", "_score", "_score_", "_docid_", "col"}) {
      if (name == reserved) fail(pos, fmt::format("'{}' cannot be used as a variable name", name));
    }
    if (findBinding(name) != nullptr) fail(pos, fmt::format("variable '{}' is already defined", name));
    return program->copyString(name);
  }

  uint32_t numericNode(uint32_t index) {
    ValueNode node = program->nodes[index];
    if (valueDecimal(node.nature)) {
      ValueNode scale;
      scale.intValue = valueDecimalScale(node.nature);
      auto scaleIndex = append(scale);
      node.kind = ValueNodeKind::FUNCTION;
      node.type = valueArray(node.type) ? ValueType::DOUBLE_ARRAY : ValueType::DOUBLE;
      node.function = ValueFunctionRegistry::decimalDecoder();
      node.opcode = ValueOpcode::NONE;
      node.children = {index, scaleIndex};
      node.childCount = 2;
      node.text = node.function->name;
    }
    node.nature = ValueNature::NUMBER;
    return append(node);
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
      node = numericNode(node);
    }
    return node;
  }

  template <class Grammar>
  uint32_t parseLet(Grammar& grammar, size_t pos) {
    std::string_view name;
    uint32_t value = 0;
    uint32_t body = 0;
    grammar.parseArguments("let", pos, 3, 3, [&](uint8_t argument) {
      if (argument == 0) {
        name = parseBindingName();
      } else if (argument == 1) {
        value = numericScalar(grammar.parseExpression(), "the value of let()");
      } else {
        bindings.push_back({name, value});
        body = numericScalar(grammar.parseExpression(), "the body of let()");
        bindings.pop_back();
      }
    });
    return body;
  }

  bool constantOperand(uint32_t index) const { return constants.scalar(index).has_value(); }

};

} // namespace luxir
