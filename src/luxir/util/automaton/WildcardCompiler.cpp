// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/automaton/WildcardCompiler.h"

#include <format>
#include <stdexcept>
#include <vector>

namespace luxir::automaton {
namespace {

[[noreturn]] void error(std::string_view message, std::string_view pattern) {
  throw std::runtime_error(std::format("{}: {}", message, pattern));
}

int32_t decode(std::string_view pattern, size_t& position) {
  unsigned char first = pattern[position++];
  if (first < 0x80) return first;
  int32_t count = (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3
                : (first & 0xf8) == 0xf0 ? 4 : 0;
  if (count == 0 || position + (size_t)count - 1 > pattern.size()) {
    error("invalid UTF-8 in wildcard pattern", pattern);
  }
  int32_t codepoint = first & ((1 << (7 - count)) - 1);
  for (int32_t i = 1; i < count; i++) {
    unsigned char byte = pattern[position++];
    if ((byte & 0xc0) != 0x80) error("invalid UTF-8 in wildcard pattern", pattern);
    codepoint = (codepoint << 6) | (byte & 0x3f);
  }
  if ((count == 2 && codepoint < 0x80) || (count == 3 && codepoint < 0x800)
      || (count == 4 && (codepoint < 0x10000 || codepoint > 0x10ffff))
      || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
    error("invalid UTF-8 in wildcard pattern", pattern);
  }
  return codepoint;
}

Automaton concatenateBalanced(std::vector<Automaton>& parts, size_t begin, size_t end,
                              Budget& budget) {
  if (end - begin == 1) return std::move(parts[begin]);
  size_t middle = begin + (end - begin) / 2;
  Automaton left = concatenateBalanced(parts, begin, middle, budget);
  Automaton right = concatenateBalanced(parts, middle, end, budget);
  return Automaton::concatenate(left, right, budget);
}

} // namespace

ByteDfa compileWildcard(std::string_view pattern, Budget& budget, const CodepointFolder* folder) {
  if (pattern.size() > 1000) error("pattern too complex", pattern);
  try {
    std::vector<Automaton> parts;
    parts.reserve(pattern.size());
    for (size_t position = 0; position < pattern.size();) {
      int32_t codepoint = decode(pattern, position);
      Automaton part;
      if (codepoint == '*') {
        part = Automaton::anyStringBytes(budget);
      } else if (codepoint == '?') {
        part = Automaton::anyChar(budget);
      } else if (codepoint == '\\') {
        if (position == pattern.size()) error("trailing escape in wildcard pattern", pattern);
        part = Automaton::literal(decode(pattern, position), folder, budget);
      } else {
        part = Automaton::literal(codepoint, folder, budget);
      }
      parts.push_back(std::move(part));
    }
    Automaton result = parts.empty() ? Automaton::epsilon(budget)
                                     : concatenateBalanced(parts, 0, parts.size(), budget);
    return ByteDfa(utf32ToUtf8(result, budget), budget);
  } catch (const std::runtime_error& e) {
    if (std::string_view(e.what()) == "pattern too complex") error("pattern too complex", pattern);
    throw;
  }
}

} // namespace luxir::automaton
