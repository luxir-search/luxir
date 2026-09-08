// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "luxir/util/Cursor.h"

namespace luxir::value::lex {

inline bool identifierStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool identifierChar(char c) {
  return identifierStart(c) || (c >= '0' && c <= '9');
}

inline bool digit(char c) {
  return c >= '0' && c <= '9';
}

inline bool identifier(std::string_view s) {
  if (s.empty() || !identifierStart(s[0])) return false;
  for (char c : s) {
    if (!identifierChar(c)) return false;
  }
  return true;
}

inline std::string_view scanIdentifier(Cursor& cur) {
  if (!identifierStart(cur.peek())) return {};
  return cur.takeWhile([](char c) { return identifierChar(c); });
}

// Cursor sits on '$'. The returned name excludes it.
inline std::string_view scanVariable(Cursor& cur) {
  cur.advance();
  return cur.takeWhile([](char c) { return identifierChar(c); });
}

// Shared numeric token grammar for every expression value position. Conversion
// remains the caller's job because query-function arguments have declared
// widths while ValueExpr preserves an int64 or double lane.
inline std::string_view scanNumber(Cursor& cur) {
  size_t start = cur.position();
  if (cur.peek() == '+' || cur.peek() == '-') cur.advance();
  bool digitsBefore = false;
  while (digit(cur.peek())) {
    digitsBefore = true;
    cur.advance();
  }
  bool digitsAfter = false;
  if (cur.consume('.')) {
    while (digit(cur.peek())) {
      digitsAfter = true;
      cur.advance();
    }
  }
  if (!digitsBefore && !digitsAfter) {
    cur.seek(start);
    return {};
  }
  if (cur.peek() == 'e' || cur.peek() == 'E') {
    size_t exponent = cur.position();
    cur.advance();
    if (cur.peek() == '+' || cur.peek() == '-') cur.advance();
    size_t exponentDigits = cur.position();
    while (digit(cur.peek())) cur.advance();
    if (cur.position() == exponentDigits) {
      cur.seek(exponent);
    }
  }
  return cur.slice(start, cur.position());
}

struct NumericLiteral {
  std::string_view text;
  bool floating = false;
  int64_t intValue = 0;
  double doubleValue = 0.0;
};

template <class Fail>
NumericLiteral parseNumericLiteral(Cursor& cur, Fail&& fail) {
  size_t pos = cur.position();
  NumericLiteral literal;
  literal.text = scanNumber(cur);
  if (literal.text.empty()) {
    fail(pos, "invalid numeric literal");
    return literal;
  }
  char next = cur.peek();
  if (!cur.atEnd() && cur.wsLen() == 0 && next != ',' && next != ')'
      && next != '+' && next != '-' && next != '*' && next != '/') {
    fail(pos, fmt::format("invalid numeric literal '{}'",
                          cur.slice(pos, cur.position() + 1)));
    return literal;
  }

  literal.floating =
      literal.text.find_first_of(".eE") != std::string_view::npos;
  std::string_view parsed = literal.text;
  if (parsed.starts_with('+')) parsed.remove_prefix(1);
  if (!literal.floating) {
    auto [ptr, ec] = std::from_chars(parsed.data(),
                                     parsed.data() + parsed.size(),
                                     literal.intValue);
    if (ec != std::errc() || ptr != parsed.data() + parsed.size()) {
      fail(pos, fmt::format("int64 literal '{}' is out of range", literal.text));
    }
    return literal;
  }

  auto [ptr, ec] = std::from_chars(parsed.data(), parsed.data() + parsed.size(),
                                   literal.doubleValue);
  if (ec != std::errc() || ptr != parsed.data() + parsed.size()
      || !std::isfinite(literal.doubleValue)) {
    fail(pos, fmt::format("double literal '{}' must be finite", literal.text));
  }
  return literal;
}

// Single and double quotes have identical semantics. Only quote, apostrophe,
// and backslash are escaped; every other backslash remains literal.
template <class Fail>
std::string_view scanQuoted(Cursor& cur, std::pmr::memory_resource& mr, Fail&& fail) {
  size_t openPos = cur.position();
  char quote = cur.peek();
  cur.advance();
  size_t start = cur.position();
  std::pmr::vector<char> buf(&mr);
  bool anyEscape = false;
  for (;;) {
    if (cur.atEnd()) fail(openPos, "unterminated quoted string");
    char c = cur.peek();
    if (c == '\\') {
      char next = cur.peekAt(1);
      if (next == '"' || next == '\'' || next == '\\') {
        buf.push_back(next);
        cur.advance(2);
        anyEscape = true;
        continue;
      }
    } else if (c == quote) {
      break;
    }
    buf.push_back(c);
    cur.advance();
  }
  std::string_view body;
  if (anyEscape) {
    char* dest = (char*)mr.allocate(buf.size(), alignof(char));
    std::copy(buf.begin(), buf.end(), dest);
    body = {dest, buf.size()};
  } else {
    body = cur.slice(start, cur.position());
  }
  cur.advance();
  return body;
}

enum class ListSeparator { COMMA, CLOSE, INVALID };

inline ListSeparator consumeListSeparator(Cursor& cur, char close = ')') {
  cur.skipWs();
  if (cur.consume(close)) return ListSeparator::CLOSE;
  if (cur.consume(',')) return ListSeparator::COMMA;
  return ListSeparator::INVALID;
}

} // namespace luxir::value::lex
