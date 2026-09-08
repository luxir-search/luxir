// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <assert.h>
#include <compare>
#include <cstring>
#include <string_view>

#include "AutomatonSeekCore.h"
#include "FilteredTermsEnum.h"
#include "luxir/util/automaton/ByteDfa.h"

namespace luxir {

struct DfaScanPlan {
  std::string_view prefix;
  std::string_view commonSuffix;
  automaton::ByteDfaView::State initialState;
};

// Byte-DFA dictionary intersection.  Outside a detected cyclic range this is
// the existing successor-driven scan.  Inside a bounded cyclic range it walks
// terms linearly, rejects by common suffix, and runs the DFA only on tail hits.
class DfaIntersectEnum final : public AutomatonSeekCore<automaton::ByteDfaView> {
  using A = automaton::ByteDfaView;
  using Core = AutomatonSeekCore<A>;
  using Successor = Core::Successor;
  using typename Core::MatchStatus;

  std::string_view commonSuffix;
  uint32_t* visitedStates;
  uint32_t visitedGeneration = 0;
  char linearUpper[PackedTerm::MAX_LEN + 1];
  int previousLiveDepth = 0;
  int linearUpperLen = 0;
  int linearUpperShared = 0;
  bool chainBroken = false;
  bool linearActive = false;
  bool totalTransitionScan = false;
  int64_t termsExaminedCount = 0;
  int64_t dpStepCount = 0;
  int64_t jumpCount = 0;
  int64_t targetComparisonCount = 0;
  int64_t linearTermCount = 0;
  int64_t suffixRejectCount = 0;

  struct SmartScan {
    int prefixLen;
    int liveDepth;
    bool chainBroken;
    int64_t examined;
    int64_t steps;
    int64_t jumps;
  };

  struct Scan : SmartScan {
    int linearUpperLen;
    int linearUpperShared;
    bool linearActive;
    int64_t linearTerms;
    int64_t suffixRejects;
  };

  SmartScan loadSmartScan() const {
    return {(int)prefix.size(), previousLiveDepth, chainBroken,
            termsExaminedCount, dpStepCount, jumpCount};
  }

  Scan loadScan() const {
    return {loadSmartScan(), linearUpperLen, linearUpperShared, linearActive,
            linearTermCount, suffixRejectCount};
  }

  void resetStack(SmartScan& s) {
    Core::resetStack();
    s.liveDepth = 0;
    s.chainBroken = true;
  }

  void store(const SmartScan& s) {
    previousLiveDepth = s.liveDepth;
    chainBroken = s.chainBroken;
    termsExaminedCount = s.examined;
    dpStepCount = s.steps;
    jumpCount = s.jumps;
  }

  void store(const Scan& s) {
    store((const SmartScan&)s);
    linearUpperLen = s.linearUpperLen;
    linearUpperShared = s.linearUpperShared;
    linearActive = s.linearActive;
    linearTermCount = s.linearTerms;
    suffixRejectCount = s.suffixRejects;
  }

  template<typename S>
  bool stop(const S& s) {
    store(s);
    return finish();
  }

  template<typename T>
  static T loadUnaligned(const char* p) {
    T value;
    memcpy(&value, p, sizeof(value));
    return value;
  }

  bool tailMatches(std::string_view term) const {
    size_t len = commonSuffix.size();
    if (term.size() < len) return false;
    const char* a = term.data() + term.size() - len;
    const char* b = commonSuffix.data();
    // The final byte is normally selective enough to reject a dictionary
    // term.  Check it before wider loads so the common path is one byte load
    // and one predictable branch.
    if (a[len - 1] != b[len - 1]) return false;
    switch (len) {
      case 1: return true;
      case 2: return a[0] == b[0];
      case 3: return loadUnaligned<uint16_t>(a) == loadUnaligned<uint16_t>(b);
      case 4: return loadUnaligned<uint16_t>(a) == loadUnaligned<uint16_t>(b)
          && a[2] == b[2];
      case 5: return loadUnaligned<uint32_t>(a) == loadUnaligned<uint32_t>(b);
      case 6: return loadUnaligned<uint32_t>(a) == loadUnaligned<uint32_t>(b)
          && a[4] == b[4];
      case 7: return loadUnaligned<uint32_t>(a) == loadUnaligned<uint32_t>(b)
          && loadUnaligned<uint16_t>(a + 4) == loadUnaligned<uint16_t>(b + 4);
      case 8: return loadUnaligned<uint32_t>(a) == loadUnaligned<uint32_t>(b)
          && loadUnaligned<uint16_t>(a + 4) == loadUnaligned<uint16_t>(b + 4)
          && a[6] == b[6];
      case 9: return loadUnaligned<uint64_t>(a) == loadUnaligned<uint64_t>(b);
      default: return memcmp(a, b, len - 1) == 0;
    }
  }

  Status acceptSmart(const A& a, SmartScan& s) {
    int shared;
    if (s.chainBroken) {
      s.chainBroken = false;
      shared = 0;
    } else {
      shared = (int)te.sharedPrefixLen();
    }
    if (shared < s.prefixLen) {
      if (!termView().starts_with(prefix)) return Status::END;
      shared = s.prefixLen;
    }
    s.examined++;
    int common = shared - s.prefixLen;
    std::string_view suffix = termView().substr((size_t)s.prefixLen);
    return matchSuffix(a, suffix, common, s.liveDepth, s.steps) == MatchStatus::ACCEPT
        ? Status::ACCEPT : Status::REJECT;
  }

  Status acceptLinear(const A& a, Scan& s) {
    std::string_view term = termView();
    if (!term.starts_with(prefix)) return Status::END;
    s.linearTerms++;
    if (!tailMatches(term)) {
      s.suffixRejects++;
      return Status::REJECT;
    }
    Core::resetStack();
    int liveDepth = 0;
    std::string_view suffix = term.substr((size_t)s.prefixLen);
    return matchSuffix(a, suffix, 0, liveDepth, s.steps) == MatchStatus::ACCEPT
        ? Status::ACCEPT : Status::REJECT;
  }

  bool termBeforeLinearUpper(Scan& s) const {
    int shared = (int)te.sharedPrefixLen();
    // The previous term was below the bound and first differed from it at
    // linearUpperShared.  Repeating beyond that byte repeats the same smaller
    // byte, so this term is below the bound without reading either string.
    if (shared > s.linearUpperShared) return true;

    std::string_view term = (std::string_view)te.term();
    int limit = std::min((int)term.size(), s.linearUpperLen);
    int common = std::min(shared, limit);
    while (common < limit
           && (uint8_t)term[(size_t)common] == (uint8_t)linearUpper[common]) {
      common++;
    }
    s.linearUpperShared = common;
    if (common == limit) return (int)term.size() < s.linearUpperLen;
    return (uint8_t)term[(size_t)common] < (uint8_t)linearUpper[common];
  }

  void planLinearRange(const A& a, std::string_view suffix, Scan& s) {
    if (commonSuffix.empty() || s.liveDepth == 0) return;
    if (++visitedGeneration == 0) {
      memset(visitedStates, 0, a.size() * sizeof(*visitedStates));
      visitedGeneration = 1;
    }
    uint32_t generation = visitedGeneration;
    visitedStates[(size_t)stack[0]] = generation;
    for (int pos = 0; pos < s.liveDepth; pos++) {
      A::State state = stack[pos + 1];
      bool repeated = visitedStates[(size_t)state] == generation;
      visitedStates[(size_t)state] = generation;
      if (!repeated) continue;
      int maxByte = a.liveByteRangeMax(stack[pos], (uint8_t)suffix[(size_t)pos]);
      assert(maxByte >= 0);
      if (s.prefixLen != 0) memcpy(linearUpper, prefix.data(), (size_t)s.prefixLen);
      if (pos != 0) {
        memcpy(linearUpper + s.prefixLen, suffix.data(), (size_t)pos);
      }
      // A 0xff transition cannot be incremented without carrying.  Bound the
      // range at 0xff itself instead; the excluded 0xff branch is resumed by
      // the successor path.  At position zero this is the near-full range
      // used by leading-star automata.
      linearUpper[s.prefixLen + pos] = (char)(uint8_t)(
          maxByte == 0xff ? 0xff : maxByte + 1);
      s.linearUpperLen = s.prefixLen + pos + 1;
      s.linearUpperShared = s.prefixLen + pos;
      s.linearActive = true;
      s.chainBroken = true;
      return;
    }
  }

  bool advanceSuccessor(const A& a, SmartScan& s) {
    std::string_view current = termView();
    assert((int)current.size() >= s.prefixLen);
    std::string_view suffix = current.substr((size_t)s.prefixLen);
    int successorLen;
    Successor kind = Core::successor(a, suffix, stack, s.liveDepth,
                                     seekBuffer + s.prefixLen, successorLen);
    if (kind == Successor::NONE) return false;
    if (!te.nextTerm()) return false;
    if (kind == Successor::ZERO_EXTENSION) return true;
    std::string_view seekTarget(seekBuffer, (size_t)(s.prefixLen + successorLen));
    targetComparisonCount++;
    if ((te.term() <=> seekTarget) >= 0) return true;
    s.jumps++;
    bool exact = te.seekForward(seekTarget);
    if (!exact) {
      targetComparisonCount++;
      if ((te.term() <=> seekTarget) < 0) {
        if (!te.nextTerm()) return false;
        assert((te.term() <=> seekTarget) >= 0);
      }
    }
    s.chainBroken = true;
    return true;
  }

  bool advanceSmart(const A& a, Scan& s) {
    std::string_view current = termView();
    assert((int)current.size() >= s.prefixLen);
    planLinearRange(a, current.substr((size_t)s.prefixLen), s);
    return advanceSuccessor(a, s);
  }

  bool seekStart(SmartScan& s) {
    resetStack(s);
    return te.seekCeil(prefix);
  }

  template<bool USE_LINEAR, bool TOTAL_TRANSITION_SCAN = false>
  [[gnu::noinline]] bool nextImpl() {
    bool advanceFirst = started;
    auto s = [&]() {
      if constexpr (USE_LINEAR) return loadScan();
      else return loadSmartScan();
    }();
    if (!started) {
      started = true;
      if constexpr (USE_LINEAR) s.linearActive = false;
      if (!seekStart(s)) return stop(s);
    } else if (advanceFirst) {
      if constexpr (USE_LINEAR) {
        if (s.linearActive) {
          if (!te.nextTerm()) return stop(s);
        } else if (!advanceSmart(automaton, s)) {
          return stop(s);
        }
      } else {
        if constexpr (TOTAL_TRANSITION_SCAN) {
          if (!te.nextTerm()) return stop(s);
        } else if (!advanceSuccessor(automaton, s)) {
          return stop(s);
        }
      }
    }

    const A a = automaton;
    for (;;) {
      if constexpr (USE_LINEAR) {
        if (s.linearActive) {
          if (!termBeforeLinearUpper(s)) {
            s.linearActive = false;
            resetStack(s);
            continue;
          }
          Status status = acceptLinear(a, s);
          if (status != Status::REJECT) {
            if (status == Status::END) return stop(s);
            store(s);
            return true;
          }
          if (!te.nextTerm()) return stop(s);
          continue;
        }
      }

      Status status = acceptSmart(a, s);
      if (status != Status::REJECT) {
        if (status == Status::END) return stop(s);
        store(s);
        return true;
      }
      if constexpr (USE_LINEAR) {
        if (!advanceSmart(a, s)) return stop(s);
      } else {
        if constexpr (TOTAL_TRANSITION_SCAN) {
          if (!te.nextTerm()) return stop(s);
        } else if (!advanceSuccessor(a, s)) {
          return stop(s);
        }
      }
    }
  }

public:
  DfaIntersectEnum(MemPool& pool, TermsEnum& te, A automaton, DfaScanPlan plan)
      : Core(pool, te, plan.prefix, automaton, plan.initialState),
        commonSuffix(plan.commonSuffix),
        visitedStates((uint32_t*)pool.alloc(automaton.size() * sizeof(*visitedStates),
                                            alignof(uint32_t))),
        totalTransitionScan(automaton.allStatesLiveOnAllBytes()) {
    memset(visitedStates, 0, automaton.size() * sizeof(*visitedStates));
  }

  bool next() final {
    if (exhausted) return false;
    if (!commonSuffix.empty()) return nextImpl<true>();
    return totalTransitionScan ? nextImpl<false, true>() : nextImpl<false>();
  }

  int64_t termsExamined() const { return termsExaminedCount; }
  int64_t dpSteps() const { return dpStepCount; }
  int64_t jumps() const { return jumpCount; }
  int64_t successorPlans() const { return termsExaminedCount; }
  int64_t targetComparisons() const { return targetComparisonCount; }
  int64_t linearTerms() const { return linearTermCount; }
  int64_t suffixRejects() const { return suffixRejectCount; }
  int64_t dfaTerms() const {
    return termsExaminedCount + linearTermCount - suffixRejectCount;
  }
};

} // namespace luxir
