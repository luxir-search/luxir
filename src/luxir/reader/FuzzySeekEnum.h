// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>

#include "AutomatonSeekEnum.h"
#include "LevenshteinAutomaton.h"

namespace luxir {

// Fuzzy-specific policy over the shared automaton seek driver: exact-prefix
// bound, matchability guard, and edit-distance score.  Lengths in the score
// are code-point units, like the distance.
class FuzzySeekEnum final : public AutomatonSeekEnum<LevenshteinAutomaton> {
  using Base = AutomatonSeekEnum<LevenshteinAutomaton>;
  int n;            // query suffix units
  int prefixUnits;
  bool prefixMode;
  bool matchable;
  float score = 1.0f;

  static std::span<const uint32_t> decodeUnits(MemPool& pool, std::string_view s) {
    if (s.empty()) return {};
    auto* units = (uint32_t*)pool.alloc(s.size() * sizeof(uint32_t), alignof(uint32_t));
    return {units, (size_t)fuzzy_units::decode(s, units)};
  }

  FuzzySeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix,
                std::span<const uint32_t> units, int maxEdits, bool prefixMode,
                bool transpositions)
      : Base(pool, te, prefix,
             LevenshteinAutomaton(units, maxEdits, prefixMode, transpositions)),
        n((int)units.size()), prefixUnits(fuzzy_units::count(prefix)),
        prefixMode(prefixMode),
        // A term suffix has at most as many units as bytes.
        matchable(prefix.size() <= PackedTerm::MAX_LEN
            && n <= (PackedTerm::MAX_LEN - (int)prefix.size()) + maxEdits) {}

protected:
  bool seekStart() override {
    resetStack();
    return matchable && te.seekCeil(prefix);
  }

  // An accepted term was stepped to its end, so its final state is the top of
  // the stack and the score denominator measures the whole term.
  void scoreCurrent() override {
    const auto& last = stack[termView().size() - prefix.size()];
    int distance = automaton.matchDistance(last);
    int denominator = prefixMode ? prefixUnits + n
        : prefixUnits + std::min(n, automaton.units(last));
    score = denominator != 0 ? 1.0f - (float)distance / (float)denominator : 1.0f;
  }

public:
  FuzzySeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix,
                std::string_view suffix, int maxEdits, bool prefixMode = false,
                bool transpositions = true)
      : FuzzySeekEnum(pool, te, prefix, decodeUnits(pool, suffix), maxEdits, prefixMode,
                      transpositions) {}

  float currentScore() const override { return score; }
};

} // namespace luxir
