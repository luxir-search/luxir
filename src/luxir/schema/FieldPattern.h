// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "luxir/util/screaming.h"

namespace luxir {

// Field selector match: '*' matches any (possibly empty) run of
// bytes, every other byte is literal.  Iterative single-star backtracking.
inline bool globMatch(std::string_view pattern, std::string_view name) {
  size_t p = 0, n = 0;
  size_t starP = std::string_view::npos, starN = 0;
  while (n < name.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      starP = p++;
      starN = n;
    } else if (p < pattern.size() && pattern[p] == name[n]) {
      p++;
      n++;
    } else if (starP != std::string_view::npos) {
      p = starP + 1;
      n = ++starN;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') p++;
  return p == pattern.size();
}

// Catalogs are sorted by selector name. Patterns containing "__" select
// variants; other patterns select logical roots. Gallop to the literal prefix
// and stop when it no longer holds.
template <typename T, typename Name, typename Derived, typename F>
void forEachFieldPattern(std::span<const T> catalog, std::string_view pattern,
                         Name&& nameOf, Derived&& derivedOf, F&& visit) {
  bool derived = pattern.find("__") != std::string_view::npos;
  std::string_view prefix = pattern.substr(0, pattern.find('*'));
  int64_t start = screaming::gallopLowerBound(0, (int64_t)catalog.size(), prefix,
      [&](int64_t i) { return std::string_view(nameOf(catalog[i])); });
  for (size_t i = (size_t)start; i < catalog.size(); i++) {
    std::string_view name = nameOf(catalog[i]);
    if (!name.starts_with(prefix)) break;
    if (derivedOf(catalog[i]) == derived && globMatch(pattern, name)) {
      visit(catalog[i]);
    }
  }
}

} // namespace luxir
