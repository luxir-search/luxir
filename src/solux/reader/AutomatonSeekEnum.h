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
  char successorBuffer[PackedTerm::MAX_LEN + 1];
  int previousLiveDepth = 0;
  int successorLen = 0;
  // A seek lands on a term the automaton never walked to, so the shared prefix
  // the dictionary reports there is shared with a term this enum never saw.
  // Set when that happens; the next accept re-anchors instead of trusting it.
  bool chainBroken = false;
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
    previousLiveDepth = 0;
    chainBroken = true;
  }

  // stack[0..previousLiveDepth] are the live states the previous term reached,
  // so any term repeating that many of its bytes reuses them as they stand.
  // The dictionary already encodes how many bytes each term repeats from the
  // one before it, which is why nothing here compares term bytes.
  Status acceptAutomaton() {
    int shared;
    if (chainBroken) {
      // Re-anchor: walk this term from the start rather than reason about which
      // term the dictionary measured its shared prefix against.  Doing it here
      // rather than trusting the enum to report zero after a seek keeps the
      // rule inside the one class that depends on it - a wrong reuse depth
      // silently drops matches instead of failing.
      chainBroken = false;
      shared = 0;
    } else {
      shared = (int)te.sharedPrefixLen();
    }
    if (shared < (int)prefix.size()) {
      // Either nothing below the query prefix is known to be reusable (block
      // boundary, re-anchor), or the scan has walked off the end of the prefix
      // range - which is how every anchored scan terminates.
      if (!termView().starts_with(prefix)) return Status::END;
      shared = (int)prefix.size();
    }
    // Otherwise the previous term began with prefix and this one repeats at
    // least that much of it, so it does too and the check is skipped.
    termsExaminedCount++;
    int common = shared - (int)prefix.size();
    if (common > previousLiveDepth) {
      // This term repeats the previous one through the byte that killed the
      // automaton, so it dies in the same place.  Rejected without stepping and
      // without reading a term byte; previousLiveDepth still describes it.
      return Status::REJECT;
    }
    std::string_view suffix = termView().substr(prefix.size());
    int liveDepth = common;
    for (int i = common; i < (int)suffix.size(); i++) {
      typename A::State next = automaton.step(stack[i], (uint8_t)suffix[(size_t)i]);
      dpStepCount++;
      if (!automaton.canMatch(next)) { liveDepth = i; break; }
      stack[i + 1] = next;
      liveDepth = i + 1;
    }
    previousLiveDepth = liveDepth;
    return liveDepth == (int)suffix.size() && automaton.isMatch(stack[suffix.size()])
        ? Status::ACCEPT : Status::REJECT;
  }

  bool advanceAutomaton() {
    // accept() classifies without moving the enum, so it is still on the term
    // whose successor is wanted: its suffix is what successor() needs.
    std::string_view current = termView();
    assert(current.size() >= prefix.size());
    std::string_view suffix = current.substr(prefix.size());
    if (!successor(automaton, suffix, stack, previousLiveDepth,
                   successorBuffer, successorLen)) return false;
    std::string_view target(successorBuffer, (size_t)successorLen);
    // A zero extension has nothing that can sort before it, so automata that
    // continue on any byte (a leading '.' or '*') skip the comparison too.
    // Only successor()'s forward branch can return one byte more than the term
    // it was given, and that branch copies the term itself into the buffer
    // first, so the leading bytes match by construction rather than by test.
    bool zeroExtension = successorLen == (int)suffix.size() + 1
        && (uint8_t)successorBuffer[suffix.size()] == 0;
    assert(!zeroExtension || memcmp(successorBuffer, suffix.data(), suffix.size()) == 0);
    // Step before seeking.  Most successors extend the current term by the
    // automaton's smallest live byte, and the only terms that can sort between
    // the two continue the current term with a SMALLER byte - usually none at
    // all - so a single step normally lands at or past the successor and a
    // seek would only re-find what the step already reached.  Seek on a real
    // undershoot, which is where the skip earns its cost.
    if (!te.nextTerm()) return false;
    if (zeroExtension) return true;
    char targetBuffer[PackedTerm::MAX_LEN + 2];
    size_t length = prefix.size() + target.size();
    assert(length <= sizeof(targetBuffer));
    if (!prefix.empty()) memcpy(targetBuffer, prefix.data(), prefix.size());
    if (!target.empty()) memcpy(targetBuffer + prefix.size(), target.data(), target.size());
    std::string_view seekTarget(targetBuffer, length);
    if ((te.term() <=> seekTarget) >= 0) return true;
    jumpCount++;
    bool exact = te.seekForward(seekTarget);
    if (!exact && (te.term() <=> seekTarget) < 0) {
      if (!te.nextTerm()) return false;
      assert((te.term() <=> seekTarget) >= 0);
    }
    chainBroken = true;  // landed on a term the automaton never walked to
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
