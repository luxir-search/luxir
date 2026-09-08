// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/automaton/RegExpParser.h"

#include <format>
#include <algorithm>
#include <stdexcept>
#include <vector>

#include "luxir/util/automaton/Utf32ToUtf8.h"

namespace luxir::automaton {
namespace {

[[noreturn]] void error(std::string_view message, size_t position, std::string_view pattern) {
  throw std::runtime_error(std::format("{} at byte {}: {}", message, position, pattern));
}

class Parser {
  std::string_view pattern;
  Budget& budget;
  const CodepointFolder* folder;
  size_t position = 0;

  int32_t next() {
    size_t start = position;
    unsigned char first = pattern[position++];
    if (first < 0x80) return first;
    int32_t count = (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3
                  : (first & 0xf8) == 0xf0 ? 4 : 0;
    if (count == 0 || position + (size_t)count - 1 > pattern.size()) error("invalid UTF-8", start, pattern);
    int32_t cp = first & ((1 << (7 - count)) - 1);
    for (int32_t i = 1; i < count; i++) {
      unsigned char byte = pattern[position++];
      if ((byte & 0xc0) != 0x80) error("invalid UTF-8", start, pattern);
      cp = (cp << 6) | (byte & 0x3f);
    }
    if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) ||
        (count == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) {
      error("invalid UTF-8", start, pattern);
    }
    return cp;
  }

  int32_t peek() {
    if (position == pattern.size()) return -1;
    size_t saved = position;
    int32_t cp = next();
    position = saved;
    return cp;
  }

  Automaton fold(std::vector<Automaton>& parts, size_t begin, size_t end, bool unionParts) {
    if (end - begin == 1) return std::move(parts[begin]);
    size_t middle = begin + (end - begin) / 2;
    Automaton left = fold(parts, begin, middle, unionParts);
    Automaton right = fold(parts, middle, end, unionParts);
    return unionParts ? Automaton::unite(left, right, budget) : Automaton::concatenate(left, right, budget);
  }

  Automaton literal(int32_t codepoint) {
    return Automaton::literal(codepoint, folder, budget);
  }

  Automaton repetition(Automaton a) {
    while (true) {
      int32_t cp = peek();
      if (cp == '?') { next(); a = Automaton::optional(a, budget); continue; }
      if (cp == '*') { next(); a = Automaton::star(a, budget); continue; }
      if (cp == '+') { next(); a = Automaton::concatenate(a, Automaton::star(a, budget), budget); continue; }
      if (cp != '{') return a;
      size_t brace = position;
      next();
      auto number = [&]() {
        size_t start = position;
        int64_t value = 0;
        while (position < pattern.size() && pattern[position] >= '0' && pattern[position] <= '9') {
          value = value * 10 + pattern[position++] - '0';
          if (value > 1000) error("repeat bound exceeds 1000", start, pattern);
        }
        if (position == start) error("expected repeat bound", position, pattern);
        return (int32_t)value;
      };
      int32_t min = number();
      int32_t max = min;
      if (peek() == ',') {
        next();
        if (peek() == '}') max = -1;
        else max = number();
      }
      if (peek() != '}') error("unterminated repeat", brace, pattern);
      next();
      if (max != -1 && max < min) error("repeat upper bound is smaller than lower bound", brace, pattern);
      if (max == -1) a = Automaton::concatenate(Automaton::repeatRange(a, min, min, budget), Automaton::star(a, budget), budget);
      else a = Automaton::repeatRange(a, min, max, budget);
    }
  }

  Automaton characterClass() {
    size_t open = position;
    next();
    bool negated = peek() == '^';
    if (negated) next();
    if (peek() == ']') {
      next();
      if (position == pattern.size()) error("empty character class", open, pattern);
      position--;
    }
    std::vector<std::pair<int32_t, int32_t>> ranges;
    bool first = true;
    while (true) {
      if (position == pattern.size()) error("unterminated character class", open, pattern);
      int32_t left = next();
      if (left == ']' && !first) break;
      if (left == '\\') {
        if (position == pattern.size()) error("trailing escape", position - 1, pattern);
        left = next();
      }
      int32_t right = left;
      if (peek() == '-' && position + 1 < pattern.size() && pattern[position + 1] != ']') {
        next();
        if (position == pattern.size()) error("unterminated character class", open, pattern);
        right = next();
        if (right == '\\') {
          if (position == pattern.size()) error("trailing escape", position - 1, pattern);
          right = next();
        }
        if (right < left) error("character class range is backwards", position, pattern);
      }
      ranges.emplace_back(left, right);
      first = false;
    }
    if (ranges.empty()) error("empty character class", open, pattern);
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<int32_t, int32_t>> merged;
    for (auto [lo, hi] : ranges) {
      if (!merged.empty() && lo <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, hi);
      else merged.emplace_back(lo, hi);
    }
    if (negated) {
      std::vector<std::pair<int32_t, int32_t>> complement;
      int32_t current = 0;
      for (auto [lo, hi] : merged) {
        if (current < lo) complement.emplace_back(current, lo - 1);
        if (hi == 0x10ffff) { current = 0x110000; break; }
        current = hi + 1;
      }
      if (current <= 0x10ffff) complement.emplace_back(current, 0x10ffff);
      merged.swap(complement);
    }
    std::vector<Automaton> parts;
    for (auto [lo, hi] : merged) {
      if (lo <= 0xd7ff) parts.push_back(Automaton::charRange(lo, std::min(hi, 0xd7ff), budget));
      if (hi >= 0xe000) parts.push_back(Automaton::charRange(std::max(lo, 0xe000), hi, budget));
    }
    return fold(parts, 0, parts.size(), true);
  }

  Automaton atom() {
    size_t start = position;
    int32_t cp = next();
    if (cp == '(') {
      Automaton a = unionExpr();
      if (peek() != ')') error("unbalanced '('", start, pattern);
      next();
      return a;
    }
    if (cp == '[') { position = start; return characterClass(); }
    if (cp == '.') return Automaton::anyChar(budget);
    if (cp == '\\') {
      if (position == pattern.size()) error("trailing escape", start, pattern);
      return literal(next());
    }
    return literal(cp);
  }

  Automaton concat() {
    std::vector<Automaton> parts;
    while (position < pattern.size()) {
      int32_t cp = peek();
      if (cp == ')' || cp == '|') break;
      if (cp == '}') error("unescaped '}' must be escaped", position, pattern);
      if (cp == '*' || cp == '+' || cp == '?' || cp == '{') error("repetition has no preceding atom", position, pattern);
      parts.push_back(repetition(atom()));
    }
    return parts.empty() ? Automaton::epsilon(budget) : fold(parts, 0, parts.size(), false);
  }

  Automaton unionExpr() {
    std::vector<Automaton> parts;
    parts.push_back(concat());
    while (peek() == '|') { next(); parts.push_back(concat()); }
    return parts.size() == 1 ? std::move(parts[0]) : fold(parts, 0, parts.size(), true);
  }

public:
  Parser(std::string_view pattern, Budget& budget, const CodepointFolder* folder)
      : pattern(pattern), budget(budget), folder(folder) {}
  Automaton parse() {
    Automaton result = unionExpr();
    if (position != pattern.size()) error("unbalanced ')'", position, pattern);
    return result;
  }
};

} // namespace

ByteDfa compileRegex(std::string_view pattern, Budget& budget, const CodepointFolder* folder) {
  if (pattern.size() > 1000) error("pattern too complex", 1000, pattern);
  try {
    Parser parser(pattern, budget, folder);
    return ByteDfa(utf32ToUtf8(parser.parse(), budget), budget);
  } catch (const std::runtime_error& e) {
    if (std::string_view(e.what()) == "pattern too complex") error("pattern too complex", 0, pattern);
    throw;
  }
}

} // namespace luxir::automaton
