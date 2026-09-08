// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/automaton/Utf32ToUtf8.h"

#include <algorithm>
#include <array>
#include <vector>

namespace luxir::automaton {
namespace {

// Ported from Lucene's UTF32ToUTF8.  A codepoint range is decomposed into a
// start fringe, a middle of complete UTF-8 subtrees, and an end fringe.
class Converter {
  Automaton out;
  Budget& budget;

  static constexpr std::array<int32_t, 4> START_CODES = {0, 128, 2048, 65536};
  static constexpr std::array<int32_t, 4> END_CODES = {127, 2047, 65535, 1114111};

  static int32_t length(int32_t codepoint) {
    if (codepoint < 128) return 1;
    if (codepoint < 2048) return 2;
    if (codepoint < 65536) return 3;
    return 4;
  }

  static std::array<int32_t, 4> encode(int32_t codepoint) {
    std::array<int32_t, 4> bytes{};
    int32_t count = length(codepoint);
    for (int32_t i = count - 1; i > 0; i--) {
      bytes[(size_t)i] = 0x80 | (codepoint & 0x3f);
      codepoint >>= 6;
    }
    bytes[0] = count == 1 ? codepoint : count == 2 ? 0xc0 | codepoint
             : count == 3 ? 0xe0 | codepoint : 0xf0 | codepoint;
    return bytes;
  }

  static int32_t bitsUsed(int32_t count, int32_t position) {
    if (position != 0) return 6;
    return count == 1 ? 7 : count == 2 ? 5 : count == 3 ? 4 : 3;
  }

  static int32_t mask(int32_t bits) {
    return (1 << bits) - 1;
  }

  // Emit a first-byte range followed by `left` unrestricted continuation
  // bytes.  This is Lucene's all() helper.
  void all(int32_t start, int32_t end, int32_t min, int32_t max, int32_t left) {
    if (left == 0) {
      out.addTransition(start, min, max, end, budget, true);
      return;
    }
    int32_t state = out.addState(budget);
    out.addTransition(start, min, max, state, budget, true);
    while (--left > 0) {
      int32_t next = out.addState(budget);
      out.addTransition(state, 0x80, 0xbf, next, budget, true);
      state = next;
    }
    out.addTransition(state, 0x80, 0xbf, end, budget, true);
  }

  // doAll means this fringe owns all byte values between its fixed leading
  // byte and the next boundary.  At recursive levels it is false only when
  // build() separately emits the middle interval.
  void start(int32_t startState, int32_t endState, const std::array<int32_t, 4>& bytes,
             int32_t count, int32_t position, bool doAll) {
    int32_t used = bitsUsed(count, position);
    if (position == count - 1) {
      out.addTransition(startState, bytes[(size_t)position],
                        bytes[(size_t)position] | mask(used), endState, budget, true);
      return;
    }
    int32_t next = out.addState(budget);
    out.addTransition(startState, bytes[(size_t)position], bytes[(size_t)position],
                      next, budget, true);
    start(next, endState, bytes, count, position + 1, true);
    int32_t max = bytes[(size_t)position] | mask(used);
    if (doAll && bytes[(size_t)position] != max) {
      all(startState, endState, bytes[(size_t)position] + 1, max, count - position - 1);
    }
  }

  void end(int32_t startState, int32_t endState, const std::array<int32_t, 4>& bytes,
           int32_t count, int32_t position, bool doAll) {
    int32_t used = bitsUsed(count, position);
    if (position == count - 1) {
      out.addTransition(startState, bytes[(size_t)position] & ~mask(used),
                        bytes[(size_t)position], endState, budget, true);
      return;
    }
    int32_t min = bytes[(size_t)position] & ~mask(used);
    if (count == 2 && position == 0) min = 0xc2;
    else if (count == 3 && position == 1 && bytes[0] == 0xe0) min = 0xa0;
    else if (count == 4 && position == 1 && bytes[0] == 0xf0) min = 0x90;
    if (doAll && bytes[(size_t)position] != min) {
      all(startState, endState, min, bytes[(size_t)position] - 1, count - position - 1);
    }
    int32_t next = out.addState(budget);
    out.addTransition(startState, bytes[(size_t)position], bytes[(size_t)position],
                      next, budget, true);
    end(next, endState, bytes, count, position + 1, true);
  }

  void build(int32_t startState, int32_t endState, const std::array<int32_t, 4>& first,
             const std::array<int32_t, 4>& last, int32_t count, int32_t position) {
    if (first[(size_t)position] == last[(size_t)position]) {
      if (position == count - 1) {
        out.addTransition(startState, first[(size_t)position], last[(size_t)position],
                          endState, budget, true);
        return;
      }
      int32_t next = out.addState(budget);
      out.addTransition(startState, first[(size_t)position], first[(size_t)position],
                        next, budget, true);
      build(next, endState, first, last, count, position + 1);
      return;
    }
    if (position == count - 1) {
      out.addTransition(startState, first[(size_t)position], last[(size_t)position],
                        endState, budget, true);
      return;
    }
    start(startState, endState, first, count, position, false);
    if (last[(size_t)position] - first[(size_t)position] > 1) {
      all(startState, endState, first[(size_t)position] + 1, last[(size_t)position] - 1,
          count - position - 1);
    }
    end(startState, endState, last, count, position, false);
  }

public:
  explicit Converter(Budget& budget) : budget(budget) {}

  int32_t addState(bool accept) {
    return out.addState(budget, accept);
  }

  void addByteTransition(int32_t from, int32_t min, int32_t max, int32_t to) {
    out.addTransition(from, min, max, to, budget, true);
  }

  void edge(int32_t startState, int32_t endState, int32_t min, int32_t max) {
    // edge() deliberately partitions at UTF-8 length boundaries, so build()
    // only receives equal-length endpoints.  The cross-length Lucene branch
    // is therefore unnecessary here.
    for (int32_t count = 1; count <= 4; count++) {
      int32_t first = std::max(min, START_CODES[(size_t)count - 1]);
      int32_t last = std::min(max, END_CODES[(size_t)count - 1]);
      if (first <= last) build(startState, endState, encode(first), encode(last), count, 0);
    }
  }

  Automaton finish() {
    out.freeze(budget);
    return out;
  }
};

} // namespace

Automaton utf32ToUtf8(const Automaton& utf32, Budget& budget) {
  Converter converter(budget);
  std::vector<int32_t> stateMap((size_t)utf32.size(), -1);
  for (int32_t state = 0; state < utf32.size(); state++) {
    stateMap[(size_t)state] = converter.addState(utf32.accept(state));
  }
  for (int32_t state = 0; state < utf32.size(); state++) {
    for (Transition transition : utf32.transitionsFrom(state)) {
      if (transition.bytes) {
        converter.addByteTransition(stateMap[(size_t)state], transition.min, transition.max,
                                    stateMap[(size_t)transition.dest]);
      } else {
        if (transition.min <= 0xd7ff) {
          converter.edge(stateMap[(size_t)state], stateMap[(size_t)transition.dest], transition.min,
                         std::min(transition.max, 0xd7ff));
        }
        if (transition.max >= 0xe000) {
          converter.edge(stateMap[(size_t)state], stateMap[(size_t)transition.dest],
                         std::max(transition.min, 0xe000), transition.max);
        }
      }
    }
  }
  return Automaton::removeDeadStates(Automaton::determinize(converter.finish(), budget), budget);
}

} // namespace luxir::automaton
