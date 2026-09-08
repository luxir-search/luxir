// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/automaton/Automaton.h"

#include <algorithm>
#include <assert.h>
#include <iterator>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "luxir/util/automaton/CodepointFolder.h"

namespace luxir::automaton {

void Budget::consume(int64_t units) {
  assert(units >= 0);
  if (units > remaining) throw std::runtime_error("pattern too complex");
  remaining -= units;
}

int32_t Automaton::addState(Budget& budget, bool accept) {
  budget.consume();
  if (frozen) throw std::logic_error("automaton is frozen");
  building.emplace_back();
  accepts.push_back(accept);
  return (int32_t)building.size() - 1;
}

void Automaton::setAccept(int32_t state, bool accept) {
  accepts[(size_t)state] = accept;
}

void Automaton::addTransition(int32_t from, int32_t min, int32_t max, int32_t to,
                              Budget& budget, bool bytes) {
  budget.consume();
  if (frozen || from < 0 || to < 0 || from >= size() || to >= size() || min > max) {
    throw std::logic_error("invalid automaton transition");
  }
  building[(size_t)from].push_back({min, max, to, bytes});
}

void Automaton::freeze(Budget& budget) {
  if (frozen) return;
  offsets.assign(building.size() + 1, 0);
  for (size_t state = 0; state < building.size(); state++) {
    auto& stateTransitions = building[state];
    std::sort(stateTransitions.begin(), stateTransitions.end(),
              [](const Transition& a, const Transition& b) {
      if (a.min != b.min) return a.min < b.min;
      if (a.max != b.max) return a.max < b.max;
      return a.dest < b.dest;
    });
    offsets[state + 1] = offsets[state] + (int32_t)stateTransitions.size();
    budget.consume((int64_t)stateTransitions.size());
    transitions.insert(transitions.end(), stateTransitions.begin(), stateTransitions.end());
  }
  building.clear();
  building.shrink_to_fit();
  frozen = true;
}

std::vector<Transition> Automaton::transitionsFrom(int32_t state) const {
  if (state < 0 || state >= size()) return {};
  return {transitions.begin() + offsets[(size_t)state],
          transitions.begin() + offsets[(size_t)state + 1]};
}

Automaton Automaton::empty(Budget& budget) {
  Automaton result;
  result.addState(budget);
  result.freeze(budget);
  return result;
}

Automaton Automaton::epsilon(Budget& budget) {
  Automaton result;
  result.addState(budget, true);
  result.freeze(budget);
  return result;
}

Automaton Automaton::charRange(int32_t min, int32_t max, Budget& budget) {
  Automaton result;
  result.addState(budget);
  result.addState(budget, true);
  result.addTransition(0, min, max, 1, budget);
  result.freeze(budget);
  return result;
}

Automaton Automaton::anyChar(Budget& budget) {
  Automaton result;
  result.addState(budget);
  result.addState(budget, true);
  result.addTransition(0, 0, 0xd7ff, 1, budget);
  result.addTransition(0, 0xe000, 0x10ffff, 1, budget);
  result.freeze(budget);
  return result;
}

Automaton Automaton::anyString(Budget& budget) {
  return star(anyChar(budget), budget);
}

Automaton Automaton::literal(int32_t cp, const CodepointFolder* folder, Budget& budget) {
  if (folder == nullptr) return codepoint(cp, budget);
  int32_t folded[32];
  int32_t count = folder->fold(cp, folded, (int32_t)std::size(folded));
  if (count < 0 || count > (int32_t)std::size(folded)) {
    throw std::runtime_error("literal fold returned an invalid codepoint count");
  }
  if (count == 0) return epsilon(budget);
  Automaton result = codepoint(folded[0], budget);
  for (int32_t i = 1; i < count; i++) {
    result = concatenate(result, codepoint(folded[i], budget), budget);
  }
  return result;
}

Automaton Automaton::anyStringBytes(Budget& budget) {
  Automaton result;
  result.addState(budget, true);
  result.addTransition(0, 0, 255, 0, budget, true);
  result.freeze(budget);
  return result;
}

static void appendCopy(Automaton& out, const Automaton& in, int32_t offset, Budget& budget) {
  for (int32_t state = 0; state < in.size(); state++) out.addState(budget, in.accept(state));
  for (int32_t state = 0; state < in.size(); state++) {
    for (Transition transition : in.transitionsFrom(state)) {
      out.addTransition(offset + state, transition.min, transition.max,
                        offset + transition.dest, budget, transition.bytes);
    }
  }
}

Automaton Automaton::unite(const Automaton& a, const Automaton& b, Budget& budget) {
  Automaton result;
  result.addState(budget, a.accept(0) || b.accept(0));
  appendCopy(result, a, 1, budget);
  appendCopy(result, b, 1 + a.size(), budget);
  for (Transition transition : a.transitionsFrom(0)) {
    result.addTransition(0, transition.min, transition.max, 1 + transition.dest,
                         budget, transition.bytes);
  }
  for (Transition transition : b.transitionsFrom(0)) {
    result.addTransition(0, transition.min, transition.max, 1 + a.size() + transition.dest,
                         budget, transition.bytes);
  }
  result.freeze(budget);
  return result;
}

Automaton Automaton::concatenate(const Automaton& a, const Automaton& b, Budget& budget) {
  Automaton result;
  appendCopy(result, a, 0, budget);
  appendCopy(result, b, a.size(), budget);
  for (int32_t state = 0; state < a.size(); state++) {
    if (!a.accept(state)) continue;
    result.setAccept(state, b.accept(0));
    for (Transition transition : b.transitionsFrom(0)) {
      result.addTransition(state, transition.min, transition.max, a.size() + transition.dest,
                           budget, transition.bytes);
    }
  }
  result.freeze(budget);
  return result;
}

Automaton Automaton::optional(const Automaton& a, Budget& budget) {
  return unite(epsilon(budget), a, budget);
}

Automaton Automaton::star(const Automaton& a, Budget& budget) {
  // The fresh start has no incoming edges.  Reusing a's start would make a
  // loop back to that state accept a partial repetition.
  Automaton result;
  result.addState(budget, true);
  appendCopy(result, a, 1, budget);
  std::vector<Transition> startTransitions = a.transitionsFrom(0);
  for (Transition transition : startTransitions) {
    result.addTransition(0, transition.min, transition.max, 1 + transition.dest,
                         budget, transition.bytes);
  }
  for (int32_t state = 0; state < a.size(); state++) {
    if (!a.accept(state)) continue;
    for (Transition transition : startTransitions) {
      result.addTransition(1 + state, transition.min, transition.max, 1 + transition.dest,
                           budget, transition.bytes);
    }
  }
  result.freeze(budget);
  return result;
}

Automaton Automaton::repeatRange(const Automaton& a, int32_t min, int32_t max,
                                 Budget& budget) {
  if (min < 0 || max < min || max > 1000) throw std::runtime_error("pattern too complex");
  Automaton result = epsilon(budget);
  for (int32_t i = 0; i < min; i++) result = concatenate(result, a, budget);
  for (int32_t i = min; i < max; i++) result = concatenate(result, optional(a, budget), budget);
  return result;
}

struct StateSetHash {
  size_t operator()(const std::vector<int32_t>& states) const {
    size_t hash = states.size();
    for (int32_t state : states) hash = hash * 1315423911u + (uint32_t)state;
    return hash;
  }
};

Automaton Automaton::determinize(const Automaton& a, Budget& budget) {
  if (a.size() == 0) return empty(budget);

  using StateSet = std::vector<int32_t>;
  std::vector<StateSet> subsets{{0}};
  std::unordered_map<StateSet, int32_t, StateSetHash> subsetToState;
  subsetToState.emplace(subsets[0], 0);
  std::queue<int32_t> pending;
  pending.push(0);
  Automaton result;
  result.addState(budget, a.accept(0));

  while (!pending.empty()) {
    int32_t resultState = pending.front();
    pending.pop();
    StateSet subset = subsets[(size_t)resultState];
    std::vector<int32_t> points;

    // Split the label line wherever an NFA range starts or ends.  Every open
    // interval then has one fixed destination subset.
    for (int32_t state : subset) {
      for (Transition transition : a.transitionsFrom(state)) {
        points.push_back(transition.min);
        if (transition.max < 0x10ffff) points.push_back(transition.max + 1);
      }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());

    for (size_t point = 0; point < points.size(); point++) {
      int32_t min = points[point];
      int32_t max = point + 1 < points.size() ? points[point + 1] - 1 : 0x10ffff;
      StateSet destination;
      for (int32_t state : subset) {
        for (Transition transition : a.transitionsFrom(state)) {
          if (transition.min <= min && transition.max >= min) destination.push_back(transition.dest);
        }
      }
      if (destination.empty()) continue;
      std::sort(destination.begin(), destination.end());
      destination.erase(std::unique(destination.begin(), destination.end()), destination.end());
      budget.consume((int64_t)subset.size() + (int64_t)destination.size());

      auto [found, inserted] = subsetToState.try_emplace(destination, (int32_t)subsets.size());
      int32_t destinationState = found->second;
      if (inserted) {
        bool accept = false;
        for (int32_t state : destination) accept = accept || a.accept(state);
        subsets.push_back(std::move(destination));
        result.addState(budget, accept);
        pending.push(destinationState);
      }
      result.addTransition(resultState, min, max, destinationState, budget);
    }
  }
  result.freeze(budget);
  return result;
}

Automaton Automaton::removeDeadStates(const Automaton& a, Budget& budget) {
  std::vector<std::vector<int32_t>> reverse((size_t)a.size());
  for (int32_t state = 0; state < a.size(); state++) {
    for (Transition transition : a.transitionsFrom(state)) reverse[(size_t)transition.dest].push_back(state);
  }
  std::vector<uint8_t> live((size_t)a.size());
  std::queue<int32_t> pending;
  for (int32_t state = 0; state < a.size(); state++) {
    if (a.accept(state)) { live[(size_t)state] = true; pending.push(state); }
  }
  while (!pending.empty()) {
    int32_t state = pending.front();
    pending.pop();
    for (int32_t previous : reverse[(size_t)state]) {
      if (!live[(size_t)previous]) { live[(size_t)previous] = true; pending.push(previous); }
    }
  }
  if (!live[0]) return empty(budget);

  std::vector<int32_t> stateMap((size_t)a.size(), -1);
  Automaton result;
  for (int32_t state = 0; state < a.size(); state++) {
    if (live[(size_t)state]) { stateMap[(size_t)state] = result.size(); result.addState(budget, a.accept(state)); }
  }
  for (int32_t state = 0; state < a.size(); state++) {
    if (!live[(size_t)state]) continue;
    for (Transition transition : a.transitionsFrom(state)) {
      if (live[(size_t)transition.dest]) {
        result.addTransition(stateMap[(size_t)state], transition.min, transition.max,
                             stateMap[(size_t)transition.dest], budget, transition.bytes);
      }
    }
  }
  result.freeze(budget);
  return result;
}

bool Automaton::isEmpty(const Automaton& a) {
  std::vector<uint8_t> seen((size_t)a.size());
  std::queue<int32_t> pending;
  pending.push(0);
  seen[0] = true;
  while (!pending.empty()) {
    int32_t state = pending.front();
    pending.pop();
    if (a.accept(state)) return false;
    for (Transition transition : a.transitionsFrom(state)) {
      if (!seen[(size_t)transition.dest]) { seen[(size_t)transition.dest] = true; pending.push(transition.dest); }
    }
  }
  return true;
}

std::string Automaton::getCommonPrefix(const Automaton& a, Budget& budget) {
  std::string prefix;
  int32_t state = 0;
  while (!a.accept(state)) {
    budget.consume();
    std::vector<Transition> transitions = a.transitionsFrom(state);
    if (transitions.size() != 1 || transitions[0].min != transitions[0].max || transitions[0].min > 255) break;
    prefix.push_back((char)transitions[0].min);
    state = transitions[0].dest;
  }
  return prefix;
}

} // namespace luxir::automaton
