#pragma once

#include <algorithm>
#include <assert.h>
#include <cstring>
#include <string_view>

#include "FilteredTermsEnum.h"
#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"

namespace solux {

// Shared forward-only driver for byte automata.  DEAD is the concept-level
// sentinel returned by nextLiveByte when no outgoing byte is live.
template <typename A>
class AutomatonSeekEnum : public FilteredTermsEnum {
protected:
  static constexpr int DEAD = -1;
  std::string_view prefix;
  A automaton;
  typename A::State initialState;
  typename A::State* stack;
  char previous[PackedTerm::MAX_LEN];
  char successorBuffer[PackedTerm::MAX_LEN + 1];
  int previousLen = 0;
  int previousLiveDepth = 0;
  int successorLen = 0;
  int64_t termsExaminedCount = 0;
  int64_t dpStepCount = 0;
  int64_t jumpCount = 0;

public:
  static bool successor(const A& automaton, std::string_view term,
                        const typename A::State* states, int liveDepth,
                        char* out, int& outLen) {
    int length = (int)term.size();
    outLen = 0;
    if (liveDepth == length) {
      int byte = automaton.nextLiveByte(states[length], 0);
      if (byte != DEAD) {
        if (length != 0) memcpy(out, term.data(), (size_t)length);
        out[length] = (char)(uint8_t)byte;
        outLen = length + 1;
        return true;
      }
    }
    for (int pos = liveDepth == length ? length - 1 : liveDepth; pos >= 0; pos--) {
      uint8_t byte = (uint8_t)term[(size_t)pos];
      if (byte == 0xff) continue;
      int next = automaton.nextLiveByte(states[pos], (int)byte + 1);
      if (next != DEAD) {
        if (pos != 0) memcpy(out, term.data(), (size_t)pos);
        out[pos] = (char)(uint8_t)next;
        outLen = pos + 1;
        return true;
      }
    }
    return false;
  }

protected:

  void resetStack() {
    stack[0] = initialState;
    previousLen = 0;
    previousLiveDepth = 0;
  }

  int commonPrefixLen(std::string_view suffix) const {
    int limit = std::min({previousLen, previousLiveDepth, (int)suffix.size()});
    int i = 0;
    while (i < limit && previous[i] == suffix[(size_t)i]) i++;
    return i;
  }

  void savePrevious(std::string_view suffix) {
    assert(suffix.size() <= PackedTerm::MAX_LEN);
    if (!suffix.empty()) memcpy(previous, suffix.data(), suffix.size());
    previousLen = (int)suffix.size();
  }

  Status acceptAutomaton() {
    std::string_view term = termView();
    if (!term.starts_with(prefix)) return Status::END;
    termsExaminedCount++;
    std::string_view suffix = term.substr(prefix.size());
    int common = commonPrefixLen(suffix);
    int liveDepth = common;
    for (int i = common; i < (int)suffix.size(); i++) {
      typename A::State next = automaton.step(stack[i], (uint8_t)suffix[(size_t)i]);
      dpStepCount++;
      if (!automaton.canMatch(next)) { liveDepth = i; break; }
      stack[i + 1] = next;
      liveDepth = i + 1;
    }
    previousLiveDepth = liveDepth;
    savePrevious(suffix);
    return liveDepth == (int)suffix.size() && automaton.isMatch(stack[suffix.size()])
        ? Status::ACCEPT : Status::REJECT;
  }

  bool advanceAutomaton() {
    if (!successor(automaton, {previous, (size_t)previousLen}, stack, previousLiveDepth,
                   successorBuffer, successorLen)) return false;
    std::string_view suffix(successorBuffer, (size_t)successorLen);
    bool appendZero = successorLen == previousLen + 1
        && memcmp(successorBuffer, previous, (size_t)previousLen) == 0
        && (uint8_t)successorBuffer[previousLen] == 0;
    if (appendZero) return te.nextTerm();
    char target[PackedTerm::MAX_LEN + 2];
    size_t length = prefix.size() + suffix.size();
    assert(length <= sizeof(target));
    if (!prefix.empty()) memcpy(target, prefix.data(), prefix.size());
    if (!suffix.empty()) memcpy(target + prefix.size(), suffix.data(), suffix.size());
    std::string_view seekTarget(target, length);
    jumpCount++;
    bool exact = te.seekForward(seekTarget);
    if (!exact && (te.term() <=> seekTarget) < 0) {
      if (!te.nextTerm()) return false;
      assert((te.term() <=> seekTarget) >= 0);
    }
    return true;
  }

  bool seekStart() override { resetStack(); return te.seekCeil(prefix); }
  Status accept() override { return acceptAutomaton(); }
  bool advance() override { return advanceAutomaton(); }

public:
  AutomatonSeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix, A automaton)
      : FilteredTermsEnum(te), prefix(prefix), automaton(std::move(automaton)),
        initialState(this->automaton.start()) {
    stack = (typename A::State*)pool.alloc((PackedTerm::MAX_LEN + 1) * sizeof(typename A::State),
                                            alignof(typename A::State));
  }

  // The caller supplies the state already reached by `prefix` when the
  // automaton spans the whole term rather than just the suffix.
  AutomatonSeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix, A automaton,
                    typename A::State initialState)
      : FilteredTermsEnum(te), prefix(prefix), automaton(std::move(automaton)),
        initialState(initialState) {
    stack = (typename A::State*)pool.alloc((PackedTerm::MAX_LEN + 1) * sizeof(typename A::State),
                                            alignof(typename A::State));
  }
  int64_t termsExamined() const { return termsExaminedCount; }
  int64_t dpSteps() const { return dpStepCount; }
  int64_t jumps() const { return jumpCount; }
};

} // namespace solux
