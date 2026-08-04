#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace solux::automaton {

class Budget {
  int64_t remaining;
public:
  explicit Budget(int64_t work = 100000) : remaining(work) {}
  void consume(int64_t units = 1);
  int64_t left() const { return remaining; }
};

struct Transition {
  int32_t min;
  int32_t max;
  int32_t dest;
  bool bytes = false;
};

// An immutable, dense-state range NFA.  The builder methods are deliberately
// public: conversion code needs to preserve NFA sharing without an epsilon
// representation.
class Automaton {
  std::vector<std::vector<Transition>> building;
  std::vector<Transition> transitions;
  std::vector<int32_t> offsets;
  std::vector<uint8_t> accepts;
  bool frozen = false;

public:
  Automaton() = default;
  int32_t addState(Budget& budget, bool accept = false);
  void setAccept(int32_t state, bool accept);
  void addTransition(int32_t from, int32_t min, int32_t max, int32_t to,
                     Budget& budget, bool bytes = false);
  void freeze(Budget& budget);
  int32_t size() const { return (int32_t)accepts.size(); }
  bool accept(int32_t state) const { return accepts[(size_t)state] != 0; }
  std::vector<Transition> transitionsFrom(int32_t state) const;
  const std::vector<uint8_t>& acceptBits() const { return accepts; }

  static Automaton empty(Budget& budget);
  static Automaton epsilon(Budget& budget);
  static Automaton charRange(int32_t min, int32_t max, Budget& budget);
  static Automaton codepoint(int32_t cp, Budget& budget) { return charRange(cp, cp, budget); }
  static Automaton anyChar(Budget& budget);
  static Automaton anyString(Budget& budget);
  static Automaton anyStringBytes(Budget& budget);
  static Automaton concatenate(const Automaton& a, const Automaton& b, Budget& budget);
  static Automaton unite(const Automaton& a, const Automaton& b, Budget& budget);
  static Automaton optional(const Automaton& a, Budget& budget);
  static Automaton star(const Automaton& a, Budget& budget);
  static Automaton repeatRange(const Automaton& a, int32_t min, int32_t max, Budget& budget);
  static Automaton determinize(const Automaton& a, Budget& budget);
  static Automaton removeDeadStates(const Automaton& a, Budget& budget);
  static bool isEmpty(const Automaton& a);
  static std::string getCommonPrefix(const Automaton& a, Budget& budget);
};

} // namespace solux::automaton
