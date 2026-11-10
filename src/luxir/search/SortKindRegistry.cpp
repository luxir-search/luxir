// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/SortKindRegistry.h"

#include <array>
#include <cmath>
#include <stdexcept>

#include <fmt/format.h>

#include "luxir/query/FieldResolver.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/Cursor.h"
#include "luxir/value/ValueLex.h"

namespace luxir {

namespace {

// The expression language's own call names around and inside a claimed call.
constexpr std::array<std::string_view, 3> RESERVED = {"min", "max", "col"};

[[noreturn]] void failExpr(std::string_view expr, std::string_view message) {
  throw RequestError(fmt::format("sort expression '{}': {}", expr, message));
}

// The position of the ')' closing the '(' at `open`, skipping quoted
// strings; npos when it is unbalanced.
size_t closingParen(std::string_view expr, size_t open) {
  int depth = 0;
  for (size_t i = open; i < expr.size(); i++) {
    char c = expr[i];
    if (c == '"' || c == '\'') {
      for (i++; i < expr.size() && expr[i] != c; i++) {
        if (expr[i] == '\\') i++;
      }
      continue;
    }
    if (c == '(') depth++;
    if (c == ')' && --depth == 0) return i;
  }
  return std::string_view::npos;
}

}  // namespace

SortKindRegistry& SortKindRegistry::global() {
  static SortKindRegistry instance;
  return instance;
}

void SortKindRegistry::add(std::string name, SortKindMaker maker) {
  if (isFrozen) throw std::logic_error("sort kind registry is frozen; sort kinds must be added during startup");
  if (name.empty()) throw std::invalid_argument("sort kind name must not be empty");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      throw std::invalid_argument("invalid sort kind name '" + name + "'; use [a-z0-9_]+");
  }
  if (value::lex::digit(name[0]))
    throw std::invalid_argument("invalid sort kind name '" + name + "'; it must not start with a digit");
  if (std::ranges::find(RESERVED, std::string_view(name)) != RESERVED.end())
    throw std::invalid_argument("sort kind '" + name + "' is a built-in expression function");
  if (!maker) throw std::invalid_argument("sort kind '" + name + "' has no maker");
  if (makers.contains(name)) throw std::invalid_argument("duplicate sort kind '" + name + "'");
  makers.emplace(std::move(name), std::move(maker));
}

const SortKindMaker* SortKindRegistry::find(std::string_view name) const {
  auto it = makers.find(name);
  return it == makers.end() ? nullptr : &it->second;
}

std::vector<std::string> SortKindRegistry::names() const {
  std::vector<std::string> out;
  for (const auto& [key, maker] : makers) out.push_back(key);
  return out;
}

void SortKindContext::fail(std::string_view message) const { failExpr(expr, message); }

void SortKindContext::note(std::string text) const { fields.note(std::move(text)); }

void SortKindContext::arity(size_t min, size_t max) const {
  if (args.size() >= min && args.size() <= max) return;
  std::string expected = min == max ? std::to_string(min)
      : max == SIZE_MAX ? "at least " + std::to_string(min)
      : std::to_string(min) + " to " + std::to_string(max);
  fail(fmt::format("{}() takes {} argument{}, got {}", name, expected,
                   min == max && min == 1 ? "" : "s", args.size()));
}

const SortKindArg& SortKindContext::arg(size_t i) const {
  if (i >= args.size()) fail(fmt::format("{}() argument {} is missing", name, i + 1));
  return args[i];
}

const ResolvedFieldHandle& SortKindContext::field(size_t i) const {
  const SortKindArg& a = arg(i);
  if (a.kind == SortKindArg::Kind::NUMBER || !a.variable.empty()) {
    fail(fmt::format("{}() argument {} must be a field name", name, i + 1));
  }
  try {
    return fields.resolve(a.text, OpClass::VALUE, "sort");
  } catch (const RequestError& e) {
    fail(fmt::format("{}() argument {}: {}", name, i + 1, e.what()));
  }
}

double SortKindContext::number(size_t i) const {
  const SortKindArg& a = arg(i);
  if (a.kind != SortKindArg::Kind::NUMBER) {
    fail(fmt::format("{}() argument {} must be a number or numeric $variable", name, i + 1));
  }
  return a.doubleValue;
}

int64_t SortKindContext::integer(size_t i) const {
  const SortKindArg& a = arg(i);
  if (a.kind != SortKindArg::Kind::NUMBER || !a.integer) {
    fail(fmt::format("{}() argument {} must be an integer or integer $variable", name, i + 1));
  }
  return a.intValue;
}

std::string_view SortKindContext::string(size_t i) const {
  const SortKindArg& a = arg(i);
  if (a.kind != SortKindArg::Kind::STRING) {
    fail(fmt::format("{}() argument {} must be a quoted string or string $variable", name, i + 1));
  }
  return a.text;
}

std::optional<SortKindCall> parseSortKindCall(
    const SortKindRegistry& registry, std::string_view expr,
    const api::map_view<std::string_view, ::hpp_proto::indirect_view<api::Val>>& vars,
    std::pmr::memory_resource& mr) {
  Cursor cur(expr);
  cur.skipWs();
  // A call name, then '(' after optional whitespace. The value language
  // takes no space there, so this claims no expression it could read.
  auto callName = [&] {
    std::string_view name = value::lex::scanIdentifier(cur);
    cur.skipWs();
    return name.empty() || cur.peek() != '(' ? std::string_view() : name;
  };
  std::string_view name = callName();
  if (name.empty()) return std::nullopt;
  SortKindCall call;
  if (name == "min" || name == "max") {
    call.mode = name == "min" ? SortKindMode::MIN : SortKindMode::MAX;
    cur.advance();
    cur.skipWs();
    name = callName();
    if (name.empty()) return std::nullopt;
  }
  call.maker = registry.find(name);
  if (call.maker == nullptr) return std::nullopt;
  call.name = name;

  // The call must be the whole expression (inside the reducer); a claimed
  // name in a larger expression is the value language's.
  size_t open = cur.position();
  size_t close = closingParen(expr, open);
  if (close == std::string_view::npos) return std::nullopt;
  cur.seek(close + 1);
  if (call.mode != SortKindMode::SINGLE) {
    cur.skipWs();
    if (!cur.consume(')')) return std::nullopt;
  }
  cur.skipWs();
  if (!cur.atEnd()) return std::nullopt;

  auto fail = [&](size_t, std::string_view message) { failExpr(expr, message); };
  auto badArgument = [&]() {
    failExpr(expr, fmt::format("{}() argument {} must be a field name, number, quoted string or $variable",
                               name, call.args.size() + 1));
  };
  cur.seek(open + 1);
  cur.skipWs();
  if (cur.position() == close) return call;
  for (;;) {
    cur.skipWs();
    SortKindArg arg;
    char c = cur.peek();
    if (c == '$') {
      arg.variable = value::lex::scanVariable(cur);
      if (arg.variable.empty()) failExpr(expr, "'$' must be followed by a variable name");
      const auto* view = vars.find(arg.variable);
      if (view == nullptr) failExpr(expr, fmt::format("undefined variable ${}", arg.variable));
      const api::Val& value = **view;
      arg.kind = SortKindArg::Kind::NUMBER;
      if (const auto* integer = std::get_if<int64_t>(&value.kind)) {
        arg.integer = true;
        arg.intValue = *integer;
        arg.doubleValue = (double)*integer;
      } else if (const auto* floating = std::get_if<double>(&value.kind)) {
        arg.doubleValue = *floating;
      } else if (const auto* single = std::get_if<float>(&value.kind)) {
        arg.doubleValue = (double)*single;
      } else if (const auto* text = std::get_if<std::string_view>(&value.kind)) {
        arg.kind = SortKindArg::Kind::STRING;
        arg.text = *text;
      } else {
        failExpr(expr, fmt::format("variable ${} is not a number or a string", arg.variable));
      }
      if (arg.kind == SortKindArg::Kind::NUMBER && !std::isfinite(arg.doubleValue)) {
        failExpr(expr, fmt::format("variable ${} contains NaN or infinity", arg.variable));
      }
    } else if (c == '"' || c == '\'') {
      arg.kind = SortKindArg::Kind::STRING;
      arg.text = value::lex::scanQuoted(cur, mr, fail);
    } else if (c == '+' || c == '-' || c == '.' || value::lex::digit(c)) {
      auto literal = value::lex::parseNumericLiteral(cur, fail);
      arg.kind = SortKindArg::Kind::NUMBER;
      arg.text = literal.text;
      arg.integer = !literal.floating;
      arg.intValue = literal.intValue;
      arg.doubleValue = literal.floating ? literal.doubleValue : (double)literal.intValue;
    } else if (value::lex::identifierStart(c)) {
      arg.text = value::lex::scanIdentifier(cur);
    } else {
      badArgument();
    }
    switch (value::lex::consumeListSeparator(cur)) {
      case value::lex::ListSeparator::COMMA:
        call.args.push_back(arg);
        continue;
      case value::lex::ListSeparator::CLOSE:
        call.args.push_back(arg);
        return call;
      case value::lex::ListSeparator::INVALID:
        badArgument();
    }
  }
}

}  // namespace luxir
