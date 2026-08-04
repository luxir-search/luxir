#pragma once

#include <algorithm>
#include <assert.h>
#include <cstdint>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include "solux/util/automaton/Automaton.h"

namespace solux::automaton {

class ByteDfa {
public:
  using State = int32_t;
  static constexpr State DEAD = -1;

  enum class Kind { NONE, SINGLE, PREFIX, ALL, NORMAL };

  struct Range {
    uint8_t min;
    uint8_t max;
    State dest;
  };

private:
  std::vector<std::vector<Range>> ranges;
  std::vector<uint8_t> accepts;
  std::vector<int16_t> table;

  int32_t minAcceptedLength() const {
    if (ranges.empty()) return -1;
    std::vector<int32_t> distance(ranges.size(), -1);
    std::queue<State> pending;
    distance[0] = 0;
    pending.push(0);
    while (!pending.empty()) {
      State state = pending.front();
      pending.pop();
      if (accepts[(size_t)state]) return distance[(size_t)state];
      for (Range range : ranges[(size_t)state]) {
        if (distance[(size_t)range.dest] == -1) {
          distance[(size_t)range.dest] = distance[(size_t)state] + 1;
          pending.push(range.dest);
        }
      }
    }
    return -1;
  }

public:
  ByteDfa() = default;
  explicit ByteDfa(const Automaton& automaton, Budget& budget) { freeze(automaton, budget); }

  // Input must be a determinized, dead-state-free byte automaton.  The debug
  // assertions below make that query-time artifact contract explicit.
  void freeze(const Automaton& automaton, Budget& budget) {
    ranges.clear();
    accepts.clear();
    table.clear();
    if (Automaton::isEmpty(automaton)) return;

    ranges.resize((size_t)automaton.size());
    accepts = automaton.acceptBits();
    for (int32_t state = 0; state < automaton.size(); state++) {
      for (Transition transition : automaton.transitionsFrom(state)) {
        assert(transition.min >= 0 && transition.max <= 255);
        assert(transition.dest >= 0 && transition.dest < automaton.size());
        ranges[(size_t)state].push_back(
            {(uint8_t)transition.min, (uint8_t)transition.max, transition.dest});
        budget.consume();
      }
    }
    for (auto& stateRanges : ranges) {
      std::sort(stateRanges.begin(), stateRanges.end(),
                [](Range a, Range b) { return a.min < b.min; });
      for (size_t i = 1; i < stateRanges.size(); i++) assert(stateRanges[i - 1].max < stateRanges[i].min);
      std::vector<Range> merged;
      for (Range range : stateRanges) {
        if (!merged.empty() && merged.back().dest == range.dest
            && (int32_t)merged.back().max + 1 == range.min) {
          merged.back().max = range.max;
        } else {
          merged.push_back(range);
        }
      }
      stateRanges.swap(merged);
    }

    std::vector<uint8_t> reachable(ranges.size());
    std::vector<State> pending{0};
    reachable[0] = true;
    for (size_t i = 0; i < pending.size(); i++) {
      for (Range range : ranges[(size_t)pending[i]]) {
        if (!reachable[(size_t)range.dest]) {
          reachable[(size_t)range.dest] = true;
          pending.push_back(range.dest);
        }
      }
    }
    for (uint8_t stateReachable : reachable) assert(stateReachable);

    std::vector<std::vector<State>> reverse(ranges.size());
    for (size_t state = 0; state < ranges.size(); state++) {
      for (Range range : ranges[state]) reverse[(size_t)range.dest].push_back((State)state);
    }
    std::vector<uint8_t> coreachable(ranges.size());
    pending.clear();
    for (size_t state = 0; state < accepts.size(); state++) {
      if (accepts[state]) { coreachable[state] = true; pending.push_back((State)state); }
    }
    for (size_t i = 0; i < pending.size(); i++) {
      for (State previous : reverse[(size_t)pending[i]]) {
        if (!coreachable[(size_t)previous]) {
          coreachable[(size_t)previous] = true;
          pending.push_back(previous);
        }
      }
    }
    for (uint8_t stateCoreachable : coreachable) assert(stateCoreachable);

    if (ranges.size() * 256 * sizeof(int16_t) <= 128 * 1024) {
      table.assign(ranges.size() * 256, DEAD);
      for (size_t state = 0; state < ranges.size(); state++) {
        for (Range range : ranges[state]) {
          for (int32_t byte = range.min; byte <= range.max; byte++) {
            table[state * 256 + (size_t)byte] = (int16_t)range.dest;
          }
        }
      }
      budget.consume((int64_t)ranges.size() * 256);
    }
  }

  State start() const { return ranges.empty() ? DEAD : 0; }
  bool canMatch(State state) const { return state != DEAD; }
  bool isMatch(State state) const { return state != DEAD && accepts[(size_t)state]; }

  State step(State state, uint8_t byte) const {
    if (state == DEAD) return DEAD;
    if (!table.empty()) return table[(size_t)state * 256 + byte];
    for (Range range : ranges[(size_t)state]) {
      if (byte >= range.min && byte <= range.max) return range.dest;
    }
    return DEAD;
  }

  int32_t nextLiveByte(State state, int32_t byte) const {
    if (state == DEAD) return DEAD;
    for (Range range : ranges[(size_t)state]) {
      if (range.max >= byte) return std::max(byte, (int32_t)range.min);
    }
    return DEAD;
  }

  bool matches(std::string_view bytes) const {
    State state = start();
    for (unsigned char byte : bytes) {
      state = step(state, byte);
      if (state == DEAD) return false;
    }
    return isMatch(state);
  }

  size_t size() const { return ranges.size(); }
  const std::vector<Range>& transitions(State state) const { return ranges[(size_t)state]; }

  std::string commonPrefix() const {
    std::string prefix;
    State state = start();
    while (state != DEAD && !isMatch(state)) {
      const auto& stateRanges = ranges[(size_t)state];
      if (stateRanges.size() != 1 || stateRanges[0].min != stateRanges[0].max) break;
      prefix.push_back((char)stateRanges[0].min);
      state = stateRanges[0].dest;
    }
    return prefix;
  }

  Kind classify(std::string* value = nullptr) const {
    if (value != nullptr) value->clear();
    if (ranges.empty() || minAcceptedLength() > 255) return Kind::NONE;

    State state = 0;
    std::string prefix;
    while (!accepts[(size_t)state]) {
      const auto& stateRanges = ranges[(size_t)state];
      if (stateRanges.size() != 1 || stateRanges[0].min != stateRanges[0].max) return Kind::NORMAL;
      prefix.push_back((char)stateRanges[0].min);
      // A unique path longer than the term cap cannot produce an indexed term,
      // so its language is NONE even if it reaches an accept later.
      if (prefix.size() > 255) return Kind::NONE;
      state = stateRanges[0].dest;
    }

    const auto& stateRanges = ranges[(size_t)state];
    bool totalSelf = stateRanges.size() == 1 && stateRanges[0].min == 0
        && stateRanges[0].max == 255 && stateRanges[0].dest == state;
    bool totalHandoff = false;
    if (stateRanges.size() == 1 && stateRanges[0].min == 0 && stateRanges[0].max == 255
        && accepts[(size_t)stateRanges[0].dest]) {
      const auto& nextRanges = ranges[(size_t)stateRanges[0].dest];
      totalHandoff = nextRanges.size() == 1 && nextRanges[0].min == 0
          && nextRanges[0].max == 255 && nextRanges[0].dest == stateRanges[0].dest;
    }
    // We intentionally do not minimize.  Determinization can therefore leave
    // this accepting handoff before the total-loop sink; it has the same byte
    // language as a total self-loop.
    if (totalSelf || totalHandoff) {
      if (value != nullptr) *value = prefix;
      return prefix.empty() ? Kind::ALL : Kind::PREFIX;
    }
    if (stateRanges.empty()) {
      if (value != nullptr) *value = prefix;
      return Kind::SINGLE;
    }
    return Kind::NORMAL;
  }
};

} // namespace solux::automaton
