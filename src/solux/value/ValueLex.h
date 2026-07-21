#pragma once

#include <algorithm>
#include <memory_resource>
#include <string_view>
#include <vector>

#include "solux/util/Cursor.h"

namespace solux::value::lex {

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

} // namespace solux::value::lex
