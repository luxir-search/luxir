// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <assert.h>
#include <string_view>

#include "AutomatonSeekCore.h"
#include "FilteredTermsEnum.h"
#include "luxir/util/StrRef.h"

namespace luxir {

// Shared forward-only driver for byte automata.  DEAD is the concept-level
// sentinel returned by nextLiveByte when no outgoing byte is live.
template <typename A>
class AutomatonSeekEnum : public AutomatonSeekCore<A> {
  using Core = AutomatonSeekCore<A>;

public:
  using Successor = typename Core::Successor;
  using Core::successor;
  using Core::termView;

protected:
  using Core::automaton;
  using Core::exhausted;
  using Core::finish;
  using Core::initialState;
  using Core::matchSuffix;
  using typename Core::MatchStatus;
  using Core::prefix;
  using Core::seekBuffer;
  using Core::stack;
  using Core::started;
  using typename Core::Status;
  using Core::te;
  int previousLiveDepth = 0;
  // A seek lands on a term the automaton never walked to, so the shared prefix
  // the dictionary reports there is shared with a term this enum never saw.
  // Set when that happens; the next accept re-anchors instead of trusting it.
  bool chainBroken = false;
  int64_t termsExaminedCount = 0;
  int64_t dpStepCount = 0;
  int64_t jumpCount = 0;

  // The state next() keeps in locals across a whole run of rejected terms,
  // writing it back to the members only when the run ends.  Every exit from
  // next() must store() it: dpSteps() and termsExamined() are read as an exact
  // oracle for how much work a scan did.
  struct Scan {
    int prefixLen;
    int liveDepth;
    bool chainBroken;
    int64_t examined;
    int64_t steps;
    int64_t jumps;
  };

  void resetStack() {
    Core::resetStack();
    previousLiveDepth = 0;
    chainBroken = true;
  }

  // stack[0..liveDepth] are the live states the previous term reached, so any
  // term repeating that many of its bytes reuses them as they stand.  The
  // dictionary already encodes how many bytes each term repeats from the one
  // before it, which is why nothing here compares term bytes.
  Status acceptAutomaton(const A& a, Scan& s) {
    int shared;
    if (s.chainBroken) {
      // Re-anchor: walk this term from the start rather than reason about which
      // term the dictionary measured its shared prefix against.  Doing it here
      // rather than trusting the enum to report zero after a seek keeps the
      // rule inside the one class that depends on it - a wrong reuse depth
      // silently drops matches instead of failing.
      s.chainBroken = false;
      shared = 0;
    } else {
      shared = (int)te.sharedPrefixLen();
    }
    if (shared < s.prefixLen) {
      // Either nothing below the query prefix is known to be reusable (block
      // boundary, re-anchor), or the scan has walked off the end of the prefix
      // range - which is how every anchored scan terminates.
      if (!termView().starts_with(prefix)) return Status::END;
      shared = s.prefixLen;
    }
    // Otherwise the previous term began with prefix and this one repeats at
    // least that much of it, so it does too and the check is skipped.
    s.examined++;
    int common = shared - s.prefixLen;
    std::string_view suffix = termView().substr((size_t)s.prefixLen);
    return matchSuffix(a, suffix, common, s.liveDepth, s.steps) == MatchStatus::ACCEPT
        ? Status::ACCEPT : Status::REJECT;
  }

  [[gnu::always_inline]] bool advanceAutomaton(const A& a, Scan& s) {
    // acceptAutomaton() classifies without moving the enum, so it is still on
    // the term whose successor is wanted: its suffix is what successor() needs.
    std::string_view current = termView();
    assert((int)current.size() >= s.prefixLen);
    std::string_view suffix = current.substr((size_t)s.prefixLen);
    int successorLen;
    Successor kind = successor(a, suffix, stack, s.liveDepth,
                               seekBuffer + s.prefixLen, successorLen);
    if (kind == Successor::NONE) return false;
    // Step before seeking.  Most successors extend the current term by the
    // automaton's smallest live byte, and the only terms that can sort between
    // the two continue the current term with a SMALLER byte - usually none at
    // all - so a single step normally lands at or past the successor and a
    // seek would only re-find what the step already reached.  Seek on a real
    // undershoot, which is where the skip earns its cost.
    if (!te.nextTerm()) return false;
    // A zero extension has nothing that can sort before it, so the step always
    // landed at or past it and automata that continue on any byte (a leading
    // '.' or '*') skip the comparison too.
    if (kind == Successor::ZERO_EXTENSION) return true;
    std::string_view seekTarget(seekBuffer, (size_t)(s.prefixLen + successorLen));
    if ((te.term() <=> seekTarget) >= 0) return true;
    s.jumps++;
    bool exact = te.seekForward(seekTarget);
    if (!exact && (te.term() <=> seekTarget) < 0) {
      if (!te.nextTerm()) return false;
      assert((te.term() <=> seekTarget) >= 0);
    }
    s.chainBroken = true;  // landed on a term the automaton never walked to
    return true;
  }

  virtual bool seekStart() { resetStack(); return te.seekCeil(prefix); }

  // Run once per ACCEPTED term, just before next() returns it.  Per-term work a
  // subclass needs only on accepts belongs here rather than in a wrapper around
  // acceptAutomaton(), which the fused scan loop does not go through: accepts
  // are rare next to rejects, so a virtual call here costs nothing, and it is
  // the only hook the loop offers.
  virtual void scoreCurrent() {}

private:
  void store(const Scan& s) {
    previousLiveDepth = s.liveDepth;
    chainBroken = s.chainBroken;
    termsExaminedCount = s.examined;
    dpStepCount = s.steps;
    jumpCount = s.jumps;
  }

  bool stop(const Scan& s) {
    store(s);
    return finish();
  }

public:
  AutomatonSeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix, A automaton)
      : Core(pool, te, prefix, std::move(automaton)) {}

  // The caller supplies the state already reached by `prefix` when the
  // automaton spans the whole term rather than just the suffix.
  AutomatonSeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix, A automaton,
                    typename A::State initialState)
      : Core(pool, te, prefix, std::move(automaton), initialState) {}

  // The scan loop, fused: it calls acceptAutomaton()/advanceAutomaton()
  // directly, so a run of rejected terms costs no virtual dispatch and keeps
  // its whole working state in locals.
  bool next() final {
    if (exhausted) return false;
    bool advanceFirst = started;
    if (!started) {
      started = true;
      // seekStart() re-anchors the stack, so the scan state is loaded after it.
      if (!seekStart()) return finish();
    }
    // One copy of the automaton for the run: the state stack holds byte-sized
    // fields, so a store into it would otherwise force a reload of the
    // automaton on every step.
    const A a = automaton;
    Scan s{(int)prefix.size(), previousLiveDepth, chainBroken,
           termsExaminedCount, dpStepCount, jumpCount};
    if (advanceFirst && !advanceAutomaton(a, s)) return stop(s);
    for (;;) {
      Status status = acceptAutomaton(a, s);
      if (status != Status::REJECT) {
        if (status == Status::END) return stop(s);
        store(s);
        scoreCurrent();
        return true;
      }
      if (!advanceAutomaton(a, s)) return stop(s);
    }
  }

  int64_t termsExamined() const { return termsExaminedCount; }
  int64_t dpSteps() const { return dpStepCount; }
  int64_t jumps() const { return jumpCount; }
};

} // namespace luxir
