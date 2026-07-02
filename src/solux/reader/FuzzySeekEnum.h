#pragma once

#include <algorithm>
#include <assert.h>
#include <cstring>
#include <string_view>

#include "FilteredTermsEnum.h"
#include "LevenshteinAutomaton.h"
#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"

namespace solux {

// Fuzzy term enum that seeks over dead runs using a Levenshtein successor.
class FuzzySeekEnum final : public FilteredTermsEnum {
  std::string_view prefix;
  int n;
  bool prefixMode;
  bool matchable;
  LevenshteinAutomaton automaton;
  LevenshteinAutomaton::State* stack;
  char prevSuffix[PackedTerm::MAX_LEN];
  char successorSuffix[PackedTerm::MAX_LEN + 1];
  int successorSuffixLen = 0;
  int prevSuffixLen = 0;
  int prevLiveDepth = 0;
  float score = 1.0f;
  int64_t termsExaminedCount = 0;
  int64_t dpStepCount = 0;
  int64_t jumpCount = 0;

  void resetStack() {
    stack[0] = automaton.start();
    prevSuffixLen = 0;
    prevLiveDepth = 0;
  }

  int commonPrefixLen(std::string_view s) const {
    // The cap is defensive; current advancement only reuses computed live states.
    int len = std::min({prevSuffixLen, prevLiveDepth, (int)s.size()});
    int i = 0;
    while (i < len && prevSuffix[i] == s[(size_t)i]) i++;
    return i;
  }

  void saveSuffix(std::string_view s) {
    assert(s.size() <= PackedTerm::MAX_LEN);
    if (!s.empty()) memcpy(prevSuffix, s.data(), s.size());
    prevSuffixLen = (int)s.size();
  }

  std::string_view previousSuffix() const {
    return {prevSuffix, (size_t)prevSuffixLen};
  }

  std::string_view successorSuffixView() const {
    return {successorSuffix, (size_t)successorSuffixLen};
  }

  bool isDegenerateAppend(std::string_view successorSuffix) const {
    return (int)successorSuffix.size() == prevSuffixLen + 1
        && memcmp(successorSuffix.data(), prevSuffix, (size_t)prevSuffixLen) == 0
        && (uint8_t)successorSuffix[(size_t)prevSuffixLen] == 0;
  }

  static bool lessThanTarget(PackedTerm term, std::string_view target) {
    return (term <=> target) < 0;
  }

protected:
  bool seekStart() override {
    resetStack();
    return matchable && te.seekCeil(prefix);
  }

  Status accept() override {
    std::string_view t = termView();
    if (!t.starts_with(prefix)) return Status::END;

    termsExaminedCount++;
    std::string_view s = t.substr(prefix.size());
    int commonLen = commonPrefixLen(s);
    int liveDepth = commonLen;

    for (int i = commonLen; i < (int)s.size(); i++) {
      LevenshteinAutomaton::State next = automaton.step(stack[i], (uint8_t)s[(size_t)i]);
      dpStepCount++;
      if (!automaton.canMatch(next)) {
        liveDepth = i;
        break;
      }
      stack[i + 1] = next;
      liveDepth = i + 1;
    }

    prevLiveDepth = liveDepth;
    saveSuffix(s);

    if (liveDepth != (int)s.size() || !automaton.isMatch(stack[(size_t)s.size()])) {
      return Status::REJECT;
    }

    int dist = automaton.matchDistance(stack[(size_t)s.size()]);
    int denom = prefixMode ? (int)prefix.size() + n
                           : (int)prefix.size() + std::min(n, (int)s.size());
    score = denom > 0 ? 1.0f - (float)dist / (float)denom : 1.0f;
    return Status::ACCEPT;
  }

  bool advance() override {
    if (!automaton.successor(previousSuffix(), stack, prevLiveDepth,
                             successorSuffix, successorSuffixLen)) {
      return false;
    }

    std::string_view suffix = successorSuffixView();
    if (isDegenerateAppend(suffix)) {
      return te.nextTerm();
    }

    char targetBuf[PackedTerm::MAX_LEN + 2];
    size_t targetLen = prefix.size() + suffix.size();
    assert(targetLen <= sizeof(targetBuf));
    if (!prefix.empty()) memcpy(targetBuf, prefix.data(), prefix.size());
    if (!suffix.empty()) {
      memcpy(targetBuf + prefix.size(), suffix.data(), suffix.size());
    }
    std::string_view target(targetBuf, targetLen);

    jumpCount++;
    bool exact = te.seekForward(target);
    if (!exact && lessThanTarget(te.term(), target)) {
      if (!te.nextTerm()) return false;
      assert(!lessThanTarget(te.term(), target));
    }
    return true;
  }

public:
  FuzzySeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix,
                std::string_view suffix, int maxEdits, bool prefixMode = false)
    : FilteredTermsEnum(te), prefix(prefix), n((int)suffix.size()), prefixMode(prefixMode),
      matchable(prefix.size() <= PackedTerm::MAX_LEN
          && n <= (PackedTerm::MAX_LEN - (int)prefix.size()) + maxEdits),
      automaton(suffix, maxEdits, prefixMode) {
    stack = (LevenshteinAutomaton::State*)pool.alloc(
        (PackedTerm::MAX_LEN + 1) * sizeof(LevenshteinAutomaton::State),
        alignof(LevenshteinAutomaton::State));
  }

  float currentScore() const override { return score; }

  int64_t termsExamined() const { return termsExaminedCount; }
  int64_t dpSteps() const { return dpStepCount; }
  int64_t jumps() const { return jumpCount; }
};

} // namespace solux
