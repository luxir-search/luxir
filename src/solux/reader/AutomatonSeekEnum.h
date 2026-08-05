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
public:
  // What successor() found.  A ZERO_EXTENSION is the term it was given plus a
  // 0x00 byte: nothing can sort between the two, so the caller only ever has to
  // step to reach it and never reads the bytes - which is why they are not
  // written.  JUMP is a target the caller has to compare against and may seek
  // to, and is the only case that fills the output buffer.
  enum class Successor { NONE, ZERO_EXTENSION, JUMP };

protected:
  static constexpr int DEAD = -1;
  std::string_view prefix;
  A automaton;
  typename A::State initialState;
  typename A::State* stack;
  // The seek target: `prefix`, then a successor of the current term's suffix.
  // The prefix half is written once at construction and never changes, so
  // building a target only rewrites the bytes after it.
  char seekBuffer[PackedTerm::MAX_LEN + 1];
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

public:
  // `out`/`outLen` are written only for JUMP.
  static Successor successor(const A& automaton, std::string_view term,
                             const typename A::State* states, int liveDepth,
                             char* out, int& outLen) {
    int length = (int)term.size();
    outLen = 0;
    if (liveDepth == length) {
      int byte = automaton.nextLiveByte(states[length], 0);
      if (byte != DEAD) {
        // Extending by 0x00 leaves the term itself as the whole prefix of the
        // result, so reporting the kind tells the caller everything the bytes
        // would have and the copy is skipped.
        if (byte == 0) return Successor::ZERO_EXTENSION;
        if (length != 0) memcpy(out, term.data(), (size_t)length);
        out[length] = (char)(uint8_t)byte;
        outLen = length + 1;
        return Successor::JUMP;
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
        return Successor::JUMP;
      }
    }
    return Successor::NONE;
  }

protected:

  void resetStack() {
    stack[0] = initialState;
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
    if (common > s.liveDepth) {
      // This term repeats the previous one through the byte that killed the
      // automaton, so it dies in the same place.  Rejected without stepping and
      // without reading a term byte; liveDepth still describes it.
      return Status::REJECT;
    }
    std::string_view suffix = termView().substr((size_t)s.prefixLen);
    const int suffixLen = (int)suffix.size();
    const char* suffixBytes = suffix.data();
    typename A::State* const states = stack;
    int64_t steps = s.steps;
    int i = common;
    for (; i < suffixLen; i++) {
      typename A::State next = a.step(states[i], (uint8_t)suffixBytes[i]);
      steps++;
      if (!a.canMatch(next)) break;
      states[i + 1] = next;
    }
    s.steps = steps;
    // The loop leaves `i` at the depth reached: the index of the byte that
    // killed the automaton, or suffixLen when every byte stepped.
    s.liveDepth = i;
    return i == suffixLen && a.isMatch(states[suffixLen]) ? Status::ACCEPT : Status::REJECT;
  }

  bool advanceAutomaton(const A& a, Scan& s) {
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

  void init(MemPool& pool) {
    stack = (typename A::State*)pool.alloc((PackedTerm::MAX_LEN + 1) * sizeof(typename A::State),
                                            alignof(typename A::State));
    // Any term this enum can accept starts with prefix and is at most MAX_LEN
    // bytes long, so a longer prefix accepts nothing.  The guard only keeps the
    // copy in bounds for a caller that builds such an enum anyway.
    if (!prefix.empty() && prefix.size() <= PackedTerm::MAX_LEN) {
      memcpy(seekBuffer, prefix.data(), prefix.size());
    }
  }

public:
  AutomatonSeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix, A automaton)
      : FilteredTermsEnum(te), prefix(prefix), automaton(std::move(automaton)),
        initialState(this->automaton.start()) {
    init(pool);
  }

  // The caller supplies the state already reached by `prefix` when the
  // automaton spans the whole term rather than just the suffix.
  AutomatonSeekEnum(MemPool& pool, TermsEnum& te, std::string_view prefix, A automaton,
                    typename A::State initialState)
      : FilteredTermsEnum(te), prefix(prefix), automaton(std::move(automaton)),
        initialState(initialState) {
    init(pool);
  }

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

} // namespace solux
