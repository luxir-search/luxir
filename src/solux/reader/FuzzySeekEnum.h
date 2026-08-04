#pragma once

#include "AutomatonSeekEnum.h"
#include "LevenshteinAutomaton.h"

namespace solux {

// Fuzzy-specific policy over the shared automaton seek driver: exact-prefix
// bound, matchability guard, and edit-distance score.
class FuzzySeekEnum final : public AutomatonSeekEnum<LevenshteinAutomaton> {
  using Base = AutomatonSeekEnum<LevenshteinAutomaton>;
  int n;
  bool prefixMode;
  bool matchable;
  float score = 1.0f;

protected:
  bool seekStart() override {
    resetStack();
    return matchable && te.seekCeil(prefix);
  }

  Status accept() override {
    Status status = acceptAutomaton();
    if (status != Status::ACCEPT) return status;
    int distance = automaton.matchDistance(stack[previousLen]);
    int denominator = prefixMode ? (int)prefix.size() + n
        : (int)prefix.size() + std::min(n, previousLen);
    score = denominator != 0 ? 1.0f - (float)distance / (float)denominator : 1.0f;
    return Status::ACCEPT;
  }

public:
  FuzzySeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix,
                std::string_view suffix, int maxEdits, bool prefixMode = false)
      : Base(pool, te, prefix, LevenshteinAutomaton(suffix, maxEdits, prefixMode)),
        n((int)suffix.size()), prefixMode(prefixMode),
        matchable(prefix.size() <= PackedTerm::MAX_LEN
            && n <= (PackedTerm::MAX_LEN - (int)prefix.size()) + maxEdits) {}

  float currentScore() const override { return score; }
};

} // namespace solux
