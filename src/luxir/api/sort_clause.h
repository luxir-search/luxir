// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace luxir::api {

// One sort clause written as a string: "expr", "expr asc", or "expr desc".
// The last whitespace-separated token names the direction when it is asc or
// desc; surrounding whitespace is ignored. This is the grammar of the JSON
// dialect's sort-clause string form and of the HTTP `sort` URL parameter.
// Returns false when no expression remains.
inline bool splitSortClause(std::string_view text, std::string_view& expr,
                            std::optional<std::string_view>& dir) {
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && ws(text[begin])) begin++;
  while (end > begin && ws(text[end - 1])) end--;
  std::size_t tokenStart = end;
  while (tokenStart > begin && !ws(text[tokenStart - 1])) tokenStart--;
  std::string_view token = text.substr(tokenStart, end - tokenStart);
  dir.reset();
  if (token == "asc" || token == "desc") {
    dir = token;
    end = tokenStart;
    while (end > begin && ws(text[end - 1])) end--;
  }
  if (end == begin) return false;
  expr = text.substr(begin, end - begin);
  return true;
}

}  // namespace luxir::api
