// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "luxir/util/Cursor.h"
#include "luxir/value/ValueLex.h"

namespace luxir {

// Shared numeric expression grammar. Actions own leaf parsing, type
// resolution, and node construction. This class owns precedence, call
// argument lists, operator spellings, and the one nesting budget used by both
// per-document and bucket expressions.
template <class Actions>
class ValueExprGrammar {
  Actions& actions;
  Cursor& cur;
  int& budget;

  class DepthGuard {
    ValueExprGrammar& grammar;

  public:
    DepthGuard(ValueExprGrammar& grammar, size_t pos) : grammar(grammar) {
      grammar.enterDepth(pos);
    }
    ~DepthGuard() { grammar.budget++; }
  };

  class ChainDepthGuard {
    ValueExprGrammar& grammar;
    size_t charges = 0;

  public:
    explicit ChainDepthGuard(ValueExprGrammar& grammar) : grammar(grammar) {}
    void charge(size_t pos) {
      grammar.enterDepth(pos);
      charges++;
    }
    ~ChainDepthGuard() {
      grammar.budget += (int)charges;
    }
  };

  using Node = typename Actions::Node;

  void enterDepth(size_t pos) {
    if (budget <= 0) {
      actions.fail(pos, "expression nesting exceeds the supported depth");
    }
    budget--;
  }

  static std::string_view operatorFunction(char op) {
    switch (op) {
      case '+': return "add";
      case '-': return "sub";
      case '*': return "mul";
      case '/': return "div";
    }
    std::unreachable();
  }

  Node makeBinary(char op, size_t pos, Node left, Node right) {
    std::array<Node, 2> args{left, right};
    return actions.makeFunction(operatorFunction(op), pos, args);
  }

  // cond ? then : else, right-associative, below every binary operator.
  Node parseConditional() {
    Node condition = parseLogical(false);
    cur.skipWs();
    if (cur.peek() != '?') return condition;
    size_t pos = cur.position();
    DepthGuard depth(*this, pos);
    cur.advance();
    Node then = parseConditional();
    cur.skipWs();
    if (!cur.consume(':')) actions.fail(cur.position(), "expected ':' in a ? b : c");
    Node otherwise = parseConditional();
    std::array<Node, 3> args{condition, then, otherwise};
    return actions.makeFunction("if", pos, args);
  }

  // || binds more loosely than &&; both are left-associative.
  Node parseLogical(bool conjunction) {
    ChainDepthGuard depth(*this);
    Node left = conjunction ? parseComparison() : parseLogical(true);
    char op = conjunction ? '&' : '|';
    for (;;) {
      cur.skipWs();
      if (cur.peek() != op || cur.peekAt(1) != op) return left;
      size_t pos = cur.position();
      depth.charge(pos);
      cur.advance(2);
      Node right = conjunction ? parseComparison() : parseLogical(true);
      std::array<Node, 2> args{left, right};
      left = actions.makeFunction(conjunction ? "and" : "or", pos, args);
    }
  }

  // The comparison operator at the cursor, or empty. Comparisons do not chain.
  std::string_view comparisonFunction(size_t& length) const {
    char c = cur.peek();
    bool equals = cur.peekAt(1) == '=';
    length = equals ? 2 : 1;
    switch (c) {
      case '<': return equals ? "lte" : "lt";
      case '>': return equals ? "gte" : "gt";
      case '=': if (equals) return "eq"; break;
      case '!': if (equals) return "ne"; break;
    }
    return {};
  }

  Node parseComparison() {
    Node left = parseAdditive();
    cur.skipWs();
    size_t length = 0;
    std::string_view name = comparisonFunction(length);
    if (name.empty()) return left;
    size_t pos = cur.position();
    DepthGuard depth(*this, pos);
    cur.advance(length);
    Node right = parseAdditive();
    cur.skipWs();
    if (!comparisonFunction(length).empty()) {
      actions.fail(cur.position(),
                   "comparisons do not chain; combine them with && or ||");
    }
    std::array<Node, 2> args{left, right};
    return actions.makeFunction(name, pos, args);
  }

  Node parseAdditive() {
    ChainDepthGuard depth(*this);
    Node left = parseMultiplicative();
    for (;;) {
      cur.skipWs();
      char op = cur.peek();
      if (op != '+' && op != '-') return left;
      size_t pos = cur.position();
      depth.charge(pos);
      cur.advance();
      Node right = parseMultiplicative();
      left = makeBinary(op, pos, left, right);
    }
  }

  Node parseMultiplicative() {
    ChainDepthGuard depth(*this);
    Node left = parseUnary();
    for (;;) {
      cur.skipWs();
      char op = cur.peek();
      if (op != '*' && op != '/') return left;
      size_t pos = cur.position();
      depth.charge(pos);
      cur.advance();
      Node right = parseUnary();
      left = makeBinary(op, pos, left, right);
    }
  }

  Node parseUnary() {
    cur.skipWs();
    size_t pos = cur.position();
    char op = cur.peek();
    if (op == '!' && cur.peekAt(1) != '=') {
      cur.advance();
      DepthGuard depth(*this, pos);
      std::array<Node, 1> args{parseUnary()};
      return actions.makeFunction("not", pos, args);
    }
    if (op != '+' && op != '-') return parsePrimary();

    char next = cur.peekAt(1);
    bool adjacentNumber = (next >= '0' && next <= '9') || next == '.';
    if (adjacentNumber) return actions.parseAtom(*this);

    cur.advance();
    DepthGuard depth(*this, pos);
    Node child = parseUnary();
    if (op == '+') return child;
    std::array<Node, 1> args{child};
    return actions.makeFunction("neg", pos, args);
  }

  Node parsePrimary() {
    cur.skipWs();
    size_t pos = cur.position();
    if (!cur.consume('(')) return actions.parseAtom(*this);

    DepthGuard depth(*this, pos);
    Node node = parseExpression();
    cur.skipWs();
    if (!cur.consume(')')) actions.fail(cur.position(), "expected ')'");
    return node;
  }

  std::string arityError(std::string_view name, uint8_t minArguments,
                         uint8_t maxArguments) const {
    if (minArguments == maxArguments) {
      return fmt::format("{}() expects exactly {} argument{}", name,
                         minArguments, minArguments == 1 ? "" : "s");
    }
    return fmt::format("{}() expects {} to {} arguments", name,
                       minArguments, maxArguments);
  }

public:
  ValueExprGrammar(Actions& actions, Cursor& cur, int& budget)
      : actions(actions), cur(cur), budget(budget) {}

  Node parseExpression() { return parseConditional(); }

  // Used only to resolve names that exist as both a one-argument aggregate
  // and a multi-argument bucket-scalar function (currently min/max). The
  // actual argument production below remains the sole parser.
  bool hasTopLevelComma() const {
    assert(cur.peek() == '(');
    size_t ahead = 1;
    size_t nested = 0;
    char quote = '\0';
    bool escaped = false;
    while (ahead < cur.remaining()) {
      char c = cur.peekAt(ahead++);
      if (quote != '\0') {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == quote) {
          quote = '\0';
        }
        continue;
      }
      if (c == '\'' || c == '"') {
        quote = c;
      } else if (c == '(') {
        nested++;
      } else if (c == ')') {
        if (nested == 0) return false;
        nested--;
      } else if (c == ',' && nested == 0) {
        return true;
      }
    }
    return false;
  }

  template <class ParseArgument>
  uint8_t parseArguments(std::string_view name, size_t callPos,
                         uint8_t minArguments, uint8_t maxArguments,
                         ParseArgument&& parseArgument) {
    DepthGuard depth(*this, callPos);
    assert(cur.peek() == '(');
    cur.advance();
    uint8_t count = 0;
    cur.skipWs();
    if (!cur.consume(')')) {
      for (;;) {
        if (count == maxArguments) {
          actions.fail(cur.position(), arityError(
              name, minArguments, maxArguments));
        }
        parseArgument(count);
        count++;
        auto separator = value::lex::consumeListSeparator(cur);
        if (separator == value::lex::ListSeparator::CLOSE) break;
        if (separator == value::lex::ListSeparator::COMMA) {
          if (count == maxArguments) {
            actions.fail(cur.position(), arityError(
                name, minArguments, maxArguments));
          }
          cur.skipWs();
          if (cur.peek() == ')') {
            actions.fail(cur.position(), "trailing comma is not allowed");
          }
          continue;
        }
        if (count == maxArguments) {
          actions.fail(cur.position(), "expected ')'");
        }
        actions.fail(cur.position(), fmt::format(
            "expected ',' or ')' in {}(...)", name));
      }
    }
    if (count < minArguments || count > maxArguments) {
      actions.fail(callPos, arityError(name, minArguments, maxArguments));
    }
    return count;
  }

  Cursor& cursor() { return cur; }
};

} // namespace luxir
