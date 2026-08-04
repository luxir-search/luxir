#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <vector>

#include "solux/util/automaton/ByteDfa.h"
#include "solux/util/automaton/Utf32ToUtf8.h"
#include "solux/util/automaton/WildcardCompiler.h"

using namespace solux::automaton;

static bool accepts(const Automaton& automaton, std::string_view bytes) {
  std::vector<int32_t> current{0};
  for (unsigned char byte : bytes) {
    std::vector<int32_t> next;
    for (int32_t state : current) {
      for (Transition transition : automaton.transitionsFrom(state)) {
        if (transition.min <= byte && byte <= transition.max) next.push_back(transition.dest);
      }
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    current.swap(next);
  }
  for (int32_t state : current) if (automaton.accept(state)) return true;
  return false;
}

static std::string utf8(int32_t codepoint) {
  std::string bytes;
  if (codepoint < 0x80) bytes.push_back((char)codepoint);
  else if (codepoint < 0x800) {
    bytes.push_back((char)(0xc0 | (codepoint >> 6)));
    bytes.push_back((char)(0x80 | (codepoint & 0x3f)));
  } else if (codepoint < 0x10000) {
    bytes.push_back((char)(0xe0 | (codepoint >> 12)));
    bytes.push_back((char)(0x80 | ((codepoint >> 6) & 0x3f)));
    bytes.push_back((char)(0x80 | (codepoint & 0x3f)));
  } else {
    bytes.push_back((char)(0xf0 | (codepoint >> 18)));
    bytes.push_back((char)(0x80 | ((codepoint >> 12) & 0x3f)));
    bytes.push_back((char)(0x80 | ((codepoint >> 6) & 0x3f)));
    bytes.push_back((char)(0x80 | (codepoint & 0x3f)));
  }
  return bytes;
}

static void strings(std::string& current, int32_t left, std::vector<std::string>& out) {
  out.push_back(current);
  if (left == 0) return;
  for (char c : std::string_view("abc")) {
    current.push_back(c);
    strings(current, left - 1, out);
    current.pop_back();
  }
}

static Automaton randomAutomaton(std::mt19937& rng, Budget& budget, int32_t depth) {
  if (depth == 0) {
    int32_t c = 'a' + (int32_t)(rng() % 3);
    return Automaton::charRange(c, c, budget);
  }
  Automaton a = randomAutomaton(rng, budget, depth - 1);
  switch (rng() % 5) {
    case 0: return Automaton::concatenate(a, randomAutomaton(rng, budget, depth - 1), budget);
    case 1: return Automaton::unite(a, randomAutomaton(rng, budget, depth - 1), budget);
    case 2: return Automaton::optional(a, budget);
    case 3: return Automaton::star(a, budget);
    default: return Automaton::repeatRange(a, (int32_t)(rng() % 3), 2 + (int32_t)(rng() % 2), budget);
  }
}

TEST(AutomatonTest, starDoesNotAcceptPartialLoop) {
  Budget budget(1000000);
  Automaton loop;
  loop.addState(budget);
  loop.addState(budget, true);
  loop.addTransition(0, 'a', 'a', 1, budget);
  loop.addTransition(1, 'b', 'b', 0, budget);
  loop.freeze(budget);
  Automaton starred = Automaton::star(loop, budget);
  EXPECT_TRUE(accepts(starred, ""));
  EXPECT_TRUE(accepts(starred, "a"));
  EXPECT_TRUE(accepts(starred, "aba"));
  EXPECT_FALSE(accepts(starred, "ab"));
}

TEST(AutomatonTest, randomizedOperationTreeDifferential) {
  std::mt19937 rng(19840119);
  std::vector<std::string> corpus;
  std::string current;
  strings(current, 6, corpus);
  for (int32_t run = 0; run < 80; run++) {
    Budget budget(10000000);
    Automaton nfa = randomAutomaton(rng, budget, 3);
    Automaton dfa = Automaton::removeDeadStates(Automaton::determinize(nfa, budget), budget);
    for (const std::string& value : corpus) EXPECT_EQ(accepts(nfa, value), accepts(dfa, value)) << value;
  }
}

TEST(AutomatonTest, utf8ScalarBoundaries) {
  for (int32_t point : {0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff}) {
    Budget budget(1000000);
    ByteDfa dfa(utf32ToUtf8(Automaton::codepoint(point, budget), budget), budget);
    EXPECT_TRUE(dfa.matches(utf8(point))) << point;
  }
}

TEST(AutomatonTest, utf8RangeDifferential) {
  std::mt19937 rng(712367);
  const std::array<int32_t, 8> boundaries = {0, 0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0x10ffff};
  for (int32_t run = 0; run < 100; run++) {
    int32_t lo = boundaries[(size_t)(rng() % boundaries.size())];
    int32_t hi = std::min(0x10ffff, lo + (int32_t)(rng() % 400));
    if (lo <= 0xdfff && hi >= 0xd800) hi = 0xd7ff;
    Budget budget(10000000);
    ByteDfa dfa(utf32ToUtf8(Automaton::charRange(lo, hi, budget), budget), budget);
    for (int32_t cp : {lo, hi, std::max(0, lo - 1), std::min(0x10ffff, hi + 1),
                       lo + (int32_t)(rng() % (hi - lo + 1))}) {
      if (cp >= 0xd800 && cp <= 0xdfff) continue;
      EXPECT_EQ(dfa.matches(utf8(cp)), cp >= lo && cp <= hi) << lo << ".." << hi << " cp=" << cp;
    }
  }
}

static size_t utf8Length(std::string_view value, size_t position) {
  unsigned char first = value[position];
  int32_t count = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3
                : (first & 0xf8) == 0xf0 ? 4 : 0;
  if (count == 0 || position + (size_t)count > value.size()) return 0;
  for (int32_t i = 1; i < count; i++) if (((unsigned char)value[position + (size_t)i] & 0xc0) != 0x80) return 0;
  return (size_t)count;
}

static bool wildcardReference(std::string_view pattern, std::string_view value, size_t p = 0, size_t v = 0) {
  if (p == pattern.size()) return v == value.size();
  if (pattern[p] == '*') {
    for (size_t end = v; end <= value.size(); end++) if (wildcardReference(pattern, value, p + 1, end)) return true;
    return false;
  }
  if (v == value.size()) return false;
  if (pattern[p] == '?') {
    size_t count = utf8Length(value, v);
    return count != 0 && wildcardReference(pattern, value, p + 1, v + count);
  }
  if (pattern[p] == '\\' && p + 1 < pattern.size()) return pattern[p + 1] == value[v] && wildcardReference(pattern, value, p + 2, v + 1);
  return pattern[p] == value[v] && wildcardReference(pattern, value, p + 1, v + 1);
}

TEST(WildcardTest, randomizedDifferential) {
  std::mt19937 rng(9231);
  const std::string alphabet = "abc*?\\";
  const std::vector<std::string> corpus = {"", "a", "abc", "\xc3\xa9", "\xff", "a\xff", "\x80", "ab\xc3\xa9"};
  for (int32_t run = 0; run < 150; run++) {
    std::string pattern;
    for (int32_t i = 0, n = 1 + (int32_t)(rng() % 6); i < n; i++) {
      char c = alphabet[(size_t)(rng() % alphabet.size())];
      if (c == '\\') { pattern += "\\a"; } else pattern.push_back(c);
    }
    Budget budget(10000000);
    ByteDfa dfa = compileWildcard(pattern, budget);
    for (const std::string& value : corpus) EXPECT_EQ(dfa.matches(value), wildcardReference(pattern, value)) << pattern;
  }
}

TEST(WildcardTest, compileAndClassify) {
  auto compile = [](std::string_view pattern) { Budget budget(10000000); return compileWildcard(pattern, budget); };
  std::string value;
  EXPECT_EQ(compile("*").classify(&value), ByteDfa::Kind::ALL);
  EXPECT_EQ(compile("**").classify(&value), ByteDfa::Kind::ALL);
  ByteDfa prefix = compile("foo*");
  EXPECT_EQ(prefix.classify(&value), ByteDfa::Kind::PREFIX);
  EXPECT_EQ(value, "foo");
  EXPECT_TRUE(prefix.matches(std::string("foo\xff", 4)));
  EXPECT_EQ(compile("foo*bar").classify(&value), ByteDfa::Kind::NORMAL);
  EXPECT_EQ(compile("foo\\*").classify(&value), ByteDfa::Kind::SINGLE);
  EXPECT_EQ(value, "foo*");
  EXPECT_EQ(compile(std::string(300, 'a') + "*").classify(), ByteDfa::Kind::NONE);
  Budget budget(1000000);
  EXPECT_EQ(ByteDfa(Automaton::empty(budget), budget).classify(), ByteDfa::Kind::NONE);
}

TEST(WildcardTest, errorsAreClean) {
  EXPECT_THROW({ Budget budget; compileWildcard("foo\\", budget); }, std::runtime_error);
  EXPECT_THROW({ Budget budget; compileWildcard("\xff", budget); }, std::runtime_error);
  EXPECT_THROW({ Budget budget(1); compileWildcard("abcdef", budget); }, std::runtime_error);
}

TEST(ByteDfaTest, randomizedFrozenPipelines) {
  std::mt19937 rng(12345);
  for (int32_t run = 0; run < 30; run++) {
    Budget budget(1000000);
    int32_t c = 'a' + (int32_t)(rng() % 3);
    Automaton automaton = Automaton::star(Automaton::optional(Automaton::charRange(c, c, budget), budget), budget);
    ByteDfa dfa(Automaton::removeDeadStates(Automaton::determinize(automaton, budget), budget), budget);
    for (std::string_view value : {"", "a", "b", "abc"}) (void)dfa.matches(value);
  }
}
