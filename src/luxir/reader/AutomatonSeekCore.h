// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

#include "luxir/reader/FilteredTermsEnum.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// Storage and successor construction shared by the fuzzy and byte-DFA
// forward enumerators.  Their scan loops are deliberately separate: fuzzy
// needs an exact state stack for scoring, while a byte DFA can enter bounded
// linear ranges and quick-reject by common suffix.
template <typename A>
class AutomatonSeekCore : public FilteredTermsEnum {
public:
  // A ZERO_EXTENSION is the input term plus 0x00.  Nothing can sort between
  // the two, so callers need not materialize or compare the target bytes.
  enum class Successor { NONE, ZERO_EXTENSION, JUMP };

protected:
  static constexpr int DEAD = -1;
  enum class MatchStatus { ACCEPT, REJECT };
  std::string_view prefix;
  A automaton;
  typename A::State initialState;
  typename A::State* stack;
  // `prefix` is copied once; successor() only rewrites bytes after it.
  char seekBuffer[PackedTerm::MAX_LEN + 1];

  void resetStack() { stack[0] = initialState; }

  MatchStatus matchSuffix(const A& a, std::string_view suffix, int common,
                          int& liveDepth, int64_t& steps) {
    if (common > liveDepth) return MatchStatus::REJECT;
    const int length = (int)suffix.size();
    const char* suffixBytes = suffix.data();
    typename A::State* const states = stack;
    int64_t localSteps = steps;
    int i = common;
    for (; i < length; i++) {
      typename A::State next = a.step(states[i], (uint8_t)suffixBytes[i]);
      localSteps++;
      if (!a.canMatch(next)) break;
      states[i + 1] = next;
    }
    steps = localSteps;
    liveDepth = i;
    return i == length && a.isMatch(states[length])
        ? MatchStatus::ACCEPT : MatchStatus::REJECT;
  }

  void init(MemPool& pool) {
    stack = (typename A::State*)pool.alloc(
        (PackedTerm::MAX_LEN + 1) * sizeof(typename A::State),
        alignof(typename A::State));
    if (!prefix.empty() && prefix.size() <= PackedTerm::MAX_LEN) {
      memcpy(seekBuffer, prefix.data(), prefix.size());
    }
  }

  AutomatonSeekCore(MemPool& pool, TermsEnum& te, std::string_view prefix, A automaton)
      : FilteredTermsEnum(te), prefix(prefix), automaton(std::move(automaton)),
        initialState(this->automaton.start()) {
    init(pool);
  }

  AutomatonSeekCore(MemPool& pool, TermsEnum& te, std::string_view prefix, A automaton,
                    typename A::State initialState)
      : FilteredTermsEnum(te), prefix(prefix), automaton(std::move(automaton)),
        initialState(initialState) {
    init(pool);
  }

public:
  // `out` and `outLen` are written only for JUMP.
  [[gnu::always_inline]] static Successor successor(
      const A& automaton, std::string_view term,
      const typename A::State* states, int liveDepth, char* out, int& outLen) {
    int length = (int)term.size();
    outLen = 0;
    if (liveDepth == length) {
      int byte = automaton.nextLiveByte(states[length], 0);
      if (byte != DEAD) {
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
};

} // namespace luxir
