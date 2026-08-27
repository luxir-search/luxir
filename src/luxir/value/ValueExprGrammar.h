#pragma once

#include <cstddef>

#include "luxir/util/Cursor.h"

namespace luxir {

template <class Actions>
class ValueExprDepthGuard {
  Actions& actions;

public:
  ValueExprDepthGuard(Actions& actions, size_t pos) : actions(actions) {
    actions.enterDepth(pos);
  }
  ~ValueExprDepthGuard() { actions.leaveDepth(); }
};

// Shared numeric expression grammar. Actions own atom parsing, type resolution,
// and node construction; this class is the single precedence implementation
// used by both per-document ValueExpr and bucket aggregate expressions.
template <class Actions>
class ValueExprGrammar {
  Actions& actions;
  Cursor& cur;

  class ChainDepthGuard {
    Actions& actions;
    size_t charges = 0;

  public:
    explicit ChainDepthGuard(Actions& actions) : actions(actions) {}
    void charge(size_t pos) {
      actions.enterDepth(pos);
      charges++;
    }
    ~ChainDepthGuard() {
      while (charges != 0) {
        actions.leaveDepth();
        charges--;
      }
    }
  };

  using Node = typename Actions::Node;

  Node parseAdditive() {
    ChainDepthGuard depth(actions);
    Node left = parseMultiplicative();
    for (;;) {
      cur.skipWs();
      char op = cur.peek();
      if (op != '+' && op != '-') return left;
      size_t pos = cur.position();
      depth.charge(pos);
      cur.advance();
      Node right = parseMultiplicative();
      left = actions.makeBinary(op, pos, left, right);
    }
  }

  Node parseMultiplicative() {
    ChainDepthGuard depth(actions);
    Node left = parseUnary();
    for (;;) {
      cur.skipWs();
      char op = cur.peek();
      if (op != '*' && op != '/') return left;
      size_t pos = cur.position();
      depth.charge(pos);
      cur.advance();
      Node right = parseUnary();
      left = actions.makeBinary(op, pos, left, right);
    }
  }

  Node parseUnary() {
    cur.skipWs();
    size_t pos = cur.position();
    char op = cur.peek();
    if (op != '+' && op != '-') return parsePrimary();

    char next = cur.peekAt(1);
    bool adjacentNumber = (next >= '0' && next <= '9') || next == '.';
    if (adjacentNumber) return actions.parseAtom(*this);

    cur.advance();
    ValueExprDepthGuard depth(actions, pos);
    Node child = parseUnary();
    return op == '+' ? child : actions.makeUnary(op, pos, child);
  }

  Node parsePrimary() {
    cur.skipWs();
    size_t pos = cur.position();
    if (!cur.consume('(')) return actions.parseAtom(*this);

    ValueExprDepthGuard depth(actions, pos);
    Node node = parseExpression();
    cur.skipWs();
    if (!cur.consume(')')) actions.fail(cur.position(), "expected ')'");
    return node;
  }

public:
  ValueExprGrammar(Actions& actions, Cursor& cur) : actions(actions), cur(cur) {}

  Node parseExpression() { return parseAdditive(); }
  Cursor& cursor() { return cur; }
};

} // namespace luxir
