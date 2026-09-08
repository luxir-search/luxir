// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <assert.h>
#include <cstdint>
#include <queue>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "luxir/util/MemPool.h"
#include "luxir/util/automaton/Automaton.h"

namespace luxir::automaton {

enum class ByteDfaKind { NONE, SINGLE, PREFIX, ALL, NORMAL };

struct ByteDfaRange {
  uint8_t min;
  uint8_t max;
  int32_t dest;
};

// Non-owning query-time representation. Its pointed-to storage is either the
// compiler artifact's vectors or a request MemPool copy made by ByteDfa::freeze.
class ByteDfaView {
public:
  using State = int32_t;
  using Range = ByteDfaRange;
  using Kind = ByteDfaKind;
  static constexpr State DEAD = -1;

private:
  const Range* ranges = nullptr;
  const uint32_t* offsets = nullptr;
  const uint8_t* accepts = nullptr;
  const int16_t* table = nullptr;
  uint32_t stateCount = 0;

  int32_t minAcceptedLength() const {
    if (stateCount == 0) return -1;
    std::vector<int32_t> distance(stateCount, -1);
    std::queue<State> pending;
    distance[0] = 0;
    pending.push(0);
    while (!pending.empty()) {
      State state = pending.front();
      pending.pop();
      if (accepts[(size_t)state]) return distance[(size_t)state];
      for (const Range& range : transitions(state)) {
        if (distance[(size_t)range.dest] == -1) {
          distance[(size_t)range.dest] = distance[(size_t)state] + 1;
          pending.push(range.dest);
        }
      }
    }
    return -1;
  }

public:
  constexpr ByteDfaView() = default;
  constexpr ByteDfaView(const Range* ranges, const uint32_t* offsets,
                        const uint8_t* accepts, const int16_t* table,
                        uint32_t stateCount)
      : ranges(ranges), offsets(offsets), accepts(accepts), table(table),
        stateCount(stateCount) {}

  State start() const { return stateCount == 0 ? DEAD : 0; }
  bool canMatch(State state) const { return state != DEAD; }
  bool isMatch(State state) const { return state != DEAD && accepts[(size_t)state]; }

  State step(State state, uint8_t byte) const {
    if (state == DEAD) return DEAD;
    if (table != nullptr) return table[(size_t)state * 256 + byte];
    for (const Range& range : transitions(state)) {
      if (byte >= range.min && byte <= range.max) return range.dest;
    }
    return DEAD;
  }

  int32_t nextLiveByte(State state, int32_t byte) const {
    if (state == DEAD) return DEAD;
    for (const Range& range : transitions(state)) {
      if (range.max >= byte) return std::max(byte, (int32_t)range.min);
    }
    return DEAD;
  }

  int32_t liveByteRangeMax(State state, uint8_t byte) const {
    if (state == DEAD) return DEAD;
    for (const Range& range : transitions(state)) {
      if (byte >= range.min && byte <= range.max) return range.max;
    }
    return DEAD;
  }

  // True when every byte from every state stays in the live DFA.  A term can
  // then never hit DEAD, and its lexicographic automaton successor is always
  // the zero extension.  Dictionary intersection may replace construction of
  // that known successor with nextTerm() without changing the visited terms.
  bool allStatesLiveOnAllBytes() const {
    for (State state = 0; state < (State)stateCount; state++) {
      int32_t nextByte = 0;
      for (const Range& range : transitions(state)) {
        if (range.min != nextByte) return false;
        nextByte = (int32_t)range.max + 1;
      }
      if (nextByte != 256) return false;
    }
    return stateCount != 0;
  }

  bool matches(std::string_view bytes) const {
    State state = start();
    for (unsigned char byte : bytes) {
      state = step(state, byte);
      if (state == DEAD) return false;
    }
    return isMatch(state);
  }

  size_t size() const { return stateCount; }
  std::span<const Range> transitions(State state) const {
    return {ranges + offsets[(size_t)state], offsets[(size_t)state + 1] - offsets[(size_t)state]};
  }

  std::string commonPrefix() const { return commonPrefixAndState().first; }
  std::pair<std::string, State> commonPrefixAndState() const {
    std::string prefix;
    State state = start();
    while (state != DEAD && !isMatch(state)) {
      auto stateRanges = transitions(state);
      if (stateRanges.size() != 1 || stateRanges[0].min != stateRanges[0].max) break;
      prefix.push_back((char)stateRanges[0].min);
      state = stateRanges[0].dest;
    }
    return {std::move(prefix), state};
  }

  Kind classify(std::string* value = nullptr) const {
    if (value != nullptr) value->clear();
    if (stateCount == 0 || minAcceptedLength() > 255) return Kind::NONE;
    State state = 0;
    std::string prefix;
    while (!accepts[(size_t)state]) {
      auto stateRanges = transitions(state);
      if (stateRanges.size() != 1 || stateRanges[0].min != stateRanges[0].max) return Kind::NORMAL;
      prefix.push_back((char)stateRanges[0].min);
      if (prefix.size() > 255) return Kind::NONE;
      state = stateRanges[0].dest;
    }
    auto stateRanges = transitions(state);
    bool totalSelf = stateRanges.size() == 1 && stateRanges[0].min == 0
        && stateRanges[0].max == 255 && stateRanges[0].dest == state;
    bool totalHandoff = false;
    if (stateRanges.size() == 1 && stateRanges[0].min == 0 && stateRanges[0].max == 255
        && accepts[(size_t)stateRanges[0].dest]) {
      auto nextRanges = transitions(stateRanges[0].dest);
      totalHandoff = nextRanges.size() == 1 && nextRanges[0].min == 0
          && nextRanges[0].max == 255 && nextRanges[0].dest == stateRanges[0].dest;
    }
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
static_assert(std::is_trivially_destructible_v<ByteDfaView>);

class ByteDfa {
public:
  using State = ByteDfaView::State;
  using Range = ByteDfaView::Range;
  using Kind = ByteDfaView::Kind;
  static constexpr State DEAD = ByteDfaView::DEAD;

private:
  static constexpr int64_t COMMON_SUFFIX_WORK_LIMIT = 1'000'000;
  std::vector<Range> ranges;
  std::vector<uint32_t> offsets;
  std::vector<uint8_t> accepts;
  std::vector<int16_t> table;

  ByteDfaView makeView() const {
    return {ranges.data(), offsets.data(), accepts.data(), table.empty() ? nullptr : table.data(),
            (uint32_t)accepts.size()};
  }

  bool isCyclic(int64_t& work) const {
    size_t n = accepts.size();
    std::vector<uint32_t> indegree(n);
    for (State state = 0; state < (State)n; state++) {
      for (const Range& range : makeView().transitions(state)) {
        if (++work > COMMON_SUFFIX_WORK_LIMIT) return false;
        indegree[(size_t)range.dest]++;
      }
    }
    std::queue<State> pending;
    for (State state = 0; state < (State)n; state++) {
      if (indegree[(size_t)state] == 0) pending.push(state);
    }
    size_t removed = 0;
    while (!pending.empty()) {
      State state = pending.front();
      pending.pop();
      removed++;
      for (const Range& range : makeView().transitions(state)) {
        if (++work > COMMON_SUFFIX_WORK_LIMIT) return false;
        if (--indegree[(size_t)range.dest] == 0) pending.push(range.dest);
      }
    }
    return removed != n;
  }

public:
  ByteDfa() = default;
  explicit ByteDfa(const Automaton& automaton, Budget& budget) { build(automaton, budget); }

  ByteDfaView view() const { return makeView(); }

  void build(const Automaton& automaton, Budget& budget) {
    ranges.clear(); offsets.clear(); accepts.clear(); table.clear();
    if (Automaton::isEmpty(automaton)) return;
    std::vector<std::vector<Range>> stateRanges((size_t)automaton.size());
    accepts = automaton.acceptBits();
    for (int32_t state = 0; state < automaton.size(); state++) {
      for (Transition transition : automaton.transitionsFrom(state)) {
        assert(transition.min >= 0 && transition.max <= 255);
        assert(transition.dest >= 0 && transition.dest < automaton.size());
        stateRanges[(size_t)state].push_back({(uint8_t)transition.min, (uint8_t)transition.max, transition.dest});
        budget.consume();
      }
    }
    offsets.reserve(stateRanges.size() + 1);
    offsets.push_back(0);
    for (auto& state : stateRanges) {
      std::sort(state.begin(), state.end(), [](Range a, Range b) { return a.min < b.min; });
      for (size_t i = 1; i < state.size(); i++) assert(state[i - 1].max < state[i].min);
      for (Range range : state) {
        if (!ranges.empty() && offsets.back() != ranges.size()
            && ranges.back().dest == range.dest && (int32_t)ranges.back().max + 1 == range.min) {
          ranges.back().max = range.max;
        } else ranges.push_back(range);
      }
      offsets.push_back((uint32_t)ranges.size());
    }
#ifndef NDEBUG
    std::vector<uint8_t> reachable(accepts.size());
    std::vector<State> pending{0};
    reachable[0] = true;
    for (size_t i = 0; i < pending.size(); i++) {
      for (const Range& range : makeView().transitions(pending[i])) {
        if (!reachable[(size_t)range.dest]) {
          reachable[(size_t)range.dest] = true;
          pending.push_back(range.dest);
        }
      }
    }
    for (uint8_t value : reachable) assert(value);

    std::vector<std::vector<State>> reverse(accepts.size());
    for (State state = 0; state < (State)accepts.size(); state++) {
      for (const Range& range : makeView().transitions(state)) {
        reverse[(size_t)range.dest].push_back(state);
      }
    }
    std::vector<uint8_t> coreachable(accepts.size());
    pending.clear();
    for (State state = 0; state < (State)accepts.size(); state++) {
      if (accepts[(size_t)state]) {
        coreachable[(size_t)state] = true;
        pending.push_back(state);
      }
    }
    for (size_t i = 0; i < pending.size(); i++) {
      for (State previous : reverse[(size_t)pending[i]]) {
        if (!coreachable[(size_t)previous]) {
          coreachable[(size_t)previous] = true;
          pending.push_back(previous);
        }
      }
    }
    for (uint8_t value : coreachable) assert(value);
#endif
    if (accepts.size() * 256 * sizeof(int16_t) <= 128 * 1024) {
      table.assign(accepts.size() * 256, DEAD);
      for (size_t state = 0; state < accepts.size(); state++) {
        for (const Range& range : makeView().transitions((State)state)) {
          for (int32_t byte = range.min; byte <= range.max; byte++) table[state * 256 + (size_t)byte] = (int16_t)range.dest;
        }
      }
      budget.consume((int64_t)accepts.size() * 256);
    }
  }

  ByteDfaView freeze(MemPool& pool) const {
    auto copy = [&]<typename T>(const std::vector<T>& source) -> T* {
      if (source.empty()) return nullptr;
      T* destination = pool.make_arr<T>(source.size());
      std::copy(source.begin(), source.end(), destination);
      return destination;
    };
    Range* copiedRanges = copy(ranges);
    uint32_t* copiedOffsets = copy(offsets);
    uint8_t* copiedAccepts = copy(accepts);
    int16_t* copiedTable = copy(table);
    return {copiedRanges, copiedOffsets, copiedAccepts, copiedTable, (uint32_t)accepts.size()};
  }

  State start() const { return makeView().start(); }
  bool canMatch(State state) const { return makeView().canMatch(state); }
  bool isMatch(State state) const { return makeView().isMatch(state); }
  State step(State state, uint8_t byte) const { return makeView().step(state, byte); }
  int32_t nextLiveByte(State state, int32_t byte) const { return makeView().nextLiveByte(state, byte); }
  int32_t liveByteRangeMax(State state, uint8_t byte) const {
    return makeView().liveByteRangeMax(state, byte);
  }
  bool matches(std::string_view bytes) const { return makeView().matches(bytes); }
  size_t size() const { return makeView().size(); }
  std::span<const Range> transitions(State state) const { return makeView().transitions(state); }
  std::string commonPrefix() const { return makeView().commonPrefix(); }
  std::pair<std::string, State> commonPrefixAndState() const { return makeView().commonPrefixAndState(); }
  std::string commonSuffix() const {
    ByteDfaView dfa = makeView();
    if (dfa.classify() != Kind::NORMAL) return {};

    int64_t work = 0;
    if (!isCyclic(work) || work > COMMON_SUFFIX_WORK_LIMIT) return {};

    std::vector<uint8_t> states(accepts.begin(), accepts.end());
    std::vector<uint8_t> predecessors(states.size());
    std::string reversed;
    for (;;) {
      if (states[0]) break;
      std::fill(predecessors.begin(), predecessors.end(), 0);
      int byte = -1;
      for (State source = 0; source < (State)states.size(); source++) {
        for (const Range& range : dfa.transitions(source)) {
          if (++work > COMMON_SUFFIX_WORK_LIMIT) return {};
          if (!states[(size_t)range.dest]) continue;
          if (range.min != range.max) {
            std::reverse(reversed.begin(), reversed.end());
            return reversed;
          }
          if (byte == -1) byte = range.min;
          else if (byte != range.min) {
            std::reverse(reversed.begin(), reversed.end());
            return reversed;
          }
          predecessors[(size_t)source] = 1;
        }
      }
      if (byte == -1) break;
      reversed.push_back((char)(uint8_t)byte);
      states.swap(predecessors);
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
  }
  Kind classify(std::string* value = nullptr) const { return makeView().classify(value); }
};

} // namespace luxir::automaton
