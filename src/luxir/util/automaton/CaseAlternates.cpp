// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/automaton/CaseAlternates.h"

#include <algorithm>
#include <iterator>
#include <map>

#include <uni_algo/case.h>
#include <uni_algo/conv.h>
#include <uni_algo/ranges_conv.h>

#include "luxir/util/automaton/CodepointFolder.h"

namespace luxir::automaton {
namespace {

// Every cased scalar, sorted, with the slice of `members` holding its class.
struct CaseClasses {
  struct Entry {
    int32_t codepoint;
    uint32_t begin;
    uint32_t end;
  };
  std::vector<Entry> entries;
  std::vector<int32_t> members;

  CaseClasses() {
    // Inverting the fold needs one pass over all scalars; it runs once per
    // process, on the first case-insensitive pattern.
    std::map<int32_t, std::vector<int32_t>> byFold;
    for (int32_t cp = 0; cp <= 0x10ffff; cp++) {
      if (cp == 0xd800) cp = 0xe000;
      int32_t folded = (int32_t)una::codepoint::to_simple_casefold((char32_t)cp);
      if (folded != cp) byFold[folded].push_back(cp);
    }
    for (auto& [folded, cps] : byFold) {
      cps.push_back(folded);
      std::sort(cps.begin(), cps.end());
      uint32_t begin = (uint32_t)members.size();
      members.insert(members.end(), cps.begin(), cps.end());
      uint32_t end = (uint32_t)members.size();
      for (int32_t cp : cps) entries.push_back({cp, begin, end});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.codepoint < b.codepoint; });
  }

  static const CaseClasses& get() {
    static const CaseClasses classes;
    return classes;
  }
};

auto lowerBound(const CaseClasses& classes, int32_t cp) {
  return std::lower_bound(classes.entries.begin(), classes.entries.end(), cp,
                          [](const CaseClasses::Entry& e, int32_t v) { return e.codepoint < v; });
}

} // namespace

std::span<const int32_t> caseAlternates(int32_t cp) {
  const CaseClasses& classes = CaseClasses::get();
  auto it = lowerBound(classes, cp);
  if (it == classes.entries.end() || it->codepoint != cp) return {};
  return {classes.members.data() + it->begin, (size_t)(it->end - it->begin)};
}

void appendCaseAlternates(int32_t lo, int32_t hi, std::vector<std::pair<int32_t, int32_t>>& ranges) {
  const CaseClasses& classes = CaseClasses::get();
  for (auto it = lowerBound(classes, lo); it != classes.entries.end() && it->codepoint <= hi; ++it) {
    for (uint32_t i = it->begin; i < it->end; i++) {
      int32_t alternate = classes.members[i];
      if (alternate < lo || alternate > hi) ranges.emplace_back(alternate, alternate);
    }
  }
}

bool caseStable(std::string_view term, const CodepointFolder& folder) {
  if (!una::is_valid_utf8(term)) return false;
  int32_t expected[32];
  int32_t actual[32];
  for (char32_t c : una::views::utf8(term)) {
    auto alternates = caseAlternates((int32_t)c);
    if (alternates.empty()) continue;
    int32_t expectedCount = folder.fold((int32_t)c, expected, (int32_t)std::size(expected));
    for (int32_t alternate : alternates) {
      int32_t count = folder.fold(alternate, actual, (int32_t)std::size(actual));
      if (count != expectedCount || !std::equal(actual, actual + count, expected)) return false;
    }
  }
  return true;
}

} // namespace luxir::automaton
