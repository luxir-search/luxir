// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <random>
#include <string>
#include <string.h>
#include <string_view>
#include <vector>

#include "luxir/reader/LevenshteinAutomaton.h"
#include "luxir/reader/AutomatonSeekEnum.h"

using namespace luxir;

namespace {

uint8_t byteAt(std::string_view s, int i) {
  return (uint8_t)s[(size_t)i];
}

std::string bytes(std::initializer_list<int> values) {
  std::string s;
  for (int v : values) s.push_back((char)(uint8_t)v);
  return s;
}

std::string hex(std::string_view s) {
  const char* digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    if (!out.empty()) out.push_back(' ');
    out.push_back(digits[(c >> 4) & 0xf]);
    out.push_back(digits[c & 0xf]);
  }
  return out;
}

std::vector<uint32_t> units(std::string_view s) {
  std::vector<uint32_t> out(s.size());
  out.resize((size_t)fuzzy_units::decode(s, out.data()));
  return out;
}

// Owns the decoded query the automaton points at.
struct Automaton {
  std::vector<uint32_t> query;
  LevenshteinAutomaton a;
  Automaton(std::string_view q, int k, bool prefixMode, bool transpositions = true)
    : query(units(q)), a(query, k, prefixMode, transpositions) {}
};

std::string randomBytes(std::mt19937& rng, int maxLen) {
  std::uniform_int_distribution<int> lenDist(0, maxLen);
  std::uniform_int_distribution<int> byteDist(0, 255);
  std::string s;
  int len = lenDist(rng);
  for (int i = 0; i < len; i++) s.push_back((char)(uint8_t)byteDist(rng));
  return s;
}

// Units of every shape: ASCII, 2/3/4-byte code points, a stray continuation,
// an invalid byte, and a lead cut short.
const std::vector<std::string>& pieces() {
  static const std::vector<std::string> p = {
    "a", "b", "c", bytes({0xc3, 0xa9}), bytes({0xe2, 0x82, 0xac}),
    bytes({0xf0, 0x9f, 0x98, 0x80}), bytes({0x80}), bytes({0xff}), bytes({0xc3}),
  };
  return p;
}

std::string randomPieces(std::mt19937& rng, int maxUnits) {
  std::uniform_int_distribution<int> lenDist(0, maxUnits);
  std::uniform_int_distribution<int> pieceDist(0, (int)pieces().size() - 1);
  std::string s;
  int len = lenDist(rng);
  for (int i = 0; i < len; i++) s += pieces()[(size_t)pieceDist(rng)];
  return s;
}

// Full-matrix optimal string alignment distance over units, with the prefix
// mode's best distance over every term prefix.
struct RefResult {
  int dist;           // whole term
  int bestPrefix;     // min over term prefixes
  int rowMin;         // min over the last row
};

RefResult refDistance(const std::vector<uint32_t>& q, const std::vector<uint32_t>& t,
                      bool transpositions) {
  int n = (int)q.size();
  int m = (int)t.size();
  std::vector<std::vector<int>> d((size_t)m + 1, std::vector<int>((size_t)n + 1));
  for (int i = 0; i <= n; i++) d[0][(size_t)i] = i;
  int bestPrefix = d[0][(size_t)n];
  for (int j = 1; j <= m; j++) {
    d[(size_t)j][0] = j;
    for (int i = 1; i <= n; i++) {
      int sub = d[(size_t)j - 1][(size_t)i - 1] + (t[(size_t)j - 1] == q[(size_t)i - 1] ? 0 : 1);
      int v = std::min({sub, d[(size_t)j - 1][(size_t)i] + 1, d[(size_t)j][(size_t)i - 1] + 1});
      if (transpositions && i > 1 && j > 1 && t[(size_t)j - 1] == q[(size_t)i - 2]
          && t[(size_t)j - 2] == q[(size_t)i - 1]) {
        v = std::min(v, d[(size_t)j - 2][(size_t)i - 2] + 1);
      }
      d[(size_t)j][(size_t)i] = v;
    }
    bestPrefix = std::min(bestPrefix, d[(size_t)j][(size_t)n]);
  }
  return {d[(size_t)m][(size_t)n], bestPrefix,
          *std::min_element(d[(size_t)m].begin(), d[(size_t)m].end())};
}

LevenshteinAutomaton::State run(const LevenshteinAutomaton& a, std::string_view term) {
  auto s = a.start();
  for (int i = 0; i < (int)term.size(); i++) s = a.step(s, byteAt(term, i));
  return s;
}

// After every byte of the term: isMatch and matchDistance agree with the
// reference over the units decoded so far (an unfinished sequence counting as
// invalid bytes), and canMatch does too on a unit boundary.
void expectTrace(std::string_view query, std::string_view term, int k, bool prefixMode,
                 bool transpositions) {
  Automaton am(query, k, prefixMode, transpositions);
  const LevenshteinAutomaton& a = am.a;
  auto s = a.start();
  for (int j = 0; j <= (int)term.size(); j++) {
    if (j > 0) s = a.step(s, byteAt(term, j - 1));
    std::string_view seen = term.substr(0, (size_t)j);
    RefResult r = refDistance(am.query, units(seen), transpositions);
    SCOPED_TRACE("q=" + hex(query) + " t=" + hex(seen) + " k=" + std::to_string(k)
                 + " prefixMode=" + std::to_string((int)prefixMode)
                 + " transpositions=" + std::to_string((int)transpositions));
    bool refMatch = (prefixMode ? r.bestPrefix : r.dist) <= k;
    EXPECT_EQ(a.isMatch(s), refMatch);
    if (refMatch) {
      EXPECT_EQ(a.matchDistance(s), prefixMode ? r.bestPrefix : r.dist);
    }
    EXPECT_EQ(a.units(s), (int)units(seen).size());
    if (s.pendingLen == 0) {
      EXPECT_EQ(a.canMatch(s), (prefixMode && r.bestPrefix <= k) || r.rowMin <= k);
    }
  }
}

// The bytes the brute searches draw from: enough to start, continue, break,
// and complete every piece above.
const std::vector<int>& alphabet() {
  static const std::vector<int> a = {0x00, 'a', 'b', 'c', 0x7f, 0x80, 0x82, 0x98, 0x9f,
                                     0xa9, 0xac, 0xbf, 0xc3, 0xe2, 0xf0, 0xff};
  return a;
}

int compareBytes(std::string_view a, std::string_view b) {
  int len = std::min((int)a.size(), (int)b.size());
  int cmp = memcmp(a.data(), b.data(), (size_t)len);
  return cmp != 0 ? cmp : (int)a.size() - (int)b.size();
}

// Some accepted string extends `s` by at most `depth` alphabet bytes.
bool canAccept(const LevenshteinAutomaton& a, const LevenshteinAutomaton::State& s, int depth) {
  if (a.isMatch(s)) return true;
  if (depth == 0 || !a.canMatch(s)) return false;
  for (int b : alphabet()) {
    if (canAccept(a, a.step(s, (uint8_t)b), depth - 1)) return true;
  }
  return false;
}

// Smallest accepted alphabet string > target of at most maxLen bytes, by
// lexicographic DFS (a prefix sorts before its extensions).
bool bruteNextAccepted(const LevenshteinAutomaton& a, std::string_view target, int maxLen,
                       std::string& cur, const LevenshteinAutomaton::State& s,
                       std::string& out) {
  std::string maxString(cur);
  maxString.append((size_t)(maxLen - (int)cur.size()), (char)0xff);
  if (compareBytes(maxString, target) <= 0) return false;
  if (compareBytes(cur, target) > 0 && a.isMatch(s)) {
    out = cur;
    return true;
  }
  if ((int)cur.size() == maxLen || !a.canMatch(s)) return false;
  for (int b : alphabet()) {
    cur.push_back((char)(uint8_t)b);
    bool found = bruteNextAccepted(a, target, maxLen, cur, a.step(s, (uint8_t)b), out);
    cur.pop_back();
    if (found) return true;
  }
  return false;
}

std::vector<LevenshteinAutomaton::State> liveStack(const LevenshteinAutomaton& a,
                                                   std::string_view term,
                                                   int& liveDepth) {
  std::vector<LevenshteinAutomaton::State> states(term.size() + 1);
  states[0] = a.start();
  liveDepth = 0;
  for (int i = 0; i < (int)term.size(); i++) {
    auto s = a.step(states[(size_t)i], byteAt(term, i));
    if (!a.canMatch(s)) {
      liveDepth = i;
      return states;
    }
    states[(size_t)i + 1] = s;
    liveDepth = i + 1;
  }
  return states;
}

// The successor a seek jumps to must not pass any accepted string after the
// term: it is at most the smallest one the brute search finds.
void expectSuccessorSound(std::string_view query, int k, bool prefixMode, std::string_view term,
                          int maxLen) {
  Automaton am(query, k, prefixMode);
  const LevenshteinAutomaton& a = am.a;
  int liveDepth;
  auto states = liveStack(a, term, liveDepth);
  using Seek = AutomatonSeekEnum<LevenshteinAutomaton>;
  char buffer[PackedTerm::MAX_LEN + 1];
  int actualLen;
  Seek::Successor kind = Seek::successor(a, term, states.data(), liveDepth, buffer, actualLen);
  std::string actual;
  if (kind == Seek::Successor::ZERO_EXTENSION) {
    actual.assign(term);
    actual.push_back('\0');
  } else {
    actual.assign(buffer, (size_t)actualLen);
  }
  std::string cur;
  std::string expected;
  bool expectedFound = bruteNextAccepted(a, term, maxLen, cur, a.start(), expected);

  SCOPED_TRACE("q=" + hex(query) + " t=" + hex(term) + " k=" + std::to_string(k)
               + " prefixMode=" + std::to_string((int)prefixMode));
  if (kind == Seek::Successor::NONE) {
    EXPECT_FALSE(expectedFound) << "missed " << hex(expected);
    return;
  }
  EXPECT_GT(compareBytes(actual, term), 0) << hex(actual);
  if (expectedFound) {
    EXPECT_LE(compareBytes(actual, expected), 0) << hex(actual);
  }
}

} // namespace

TEST(LevenshteinAutomatonTest, DecodesUnits) {
  using U = std::vector<uint32_t>;
  constexpr uint32_t X = fuzzy_units::INVALID_UNIT;
  EXPECT_EQ(units("ab"), (U{'a', 'b'}));
  EXPECT_EQ(units(bytes({0xc3, 0xa9, 'x'})), (U{0xe9, 'x'}));
  EXPECT_EQ(units(bytes({0xe2, 0x82, 0xac})), (U{0x20ac}));
  EXPECT_EQ(units(bytes({0xf0, 0x9f, 0x98, 0x80})), (U{0x1f600}));
  // A lead cut short, overlongs, surrogates, and stray bytes are one unit each.
  EXPECT_EQ(units(bytes({0xc3, 'x'})), (U{X | 0xc3, 'x'}));
  EXPECT_EQ(units(bytes({0xe2, 0x82})), (U{X | 0xe2, X | 0x82}));
  EXPECT_EQ(units(bytes({0xc0, 0x80})), (U{X | 0xc0, X | 0x80}));
  EXPECT_EQ(units(bytes({0xed, 0xa0, 0x80})), (U{X | 0xed, X | 0xa0, X | 0x80}));
  EXPECT_EQ(units(bytes({0xff, 0xc3, 0xa9})), (U{X | 0xff, 0xe9}));
  EXPECT_EQ(fuzzy_units::count(bytes({'a', 0xc3, 0xa9, 0xff})), 3);
  EXPECT_EQ(fuzzy_units::prefixBytes(bytes({'a', 0xc3, 0xa9, 0xff}), 2), 3u);
  EXPECT_EQ(fuzzy_units::prefixBytes("ab", 9), 2u);
}

TEST(LevenshteinAutomatonTest, RowsMatchFullDpReference) {
  std::vector<std::string> queries = {
    "", "a", "ab", "abc", bytes({0x80, 0xff, 'b'}), bytes({'c', 0xc3, 0xa9}),
  };
  std::vector<std::string> terms = {
    "", "a", "ba", "abc", "acb", bytes({0x80, 0xff}), bytes({0xff, 0x00}),
    bytes({0xc3, 0xa9, 'c'}), bytes({'c', 0xc3}),
  };
  for (const auto& q : queries) {
    for (const auto& t : terms) {
      for (int k = 0; k <= 2; k++) {
        for (bool prefixMode : {false, true}) {
          for (bool transpositions : {false, true}) {
            expectTrace(q, t, k, prefixMode, transpositions);
          }
        }
      }
    }
  }

  std::mt19937 rng(0x5eed1234);
  for (int i = 0; i < 300; i++) {
    std::string q = i % 2 == 0 ? randomBytes(rng, 8) : randomPieces(rng, 6);
    std::string t = i % 3 == 0 ? randomBytes(rng, 10) : randomPieces(rng, 7);
    for (int k = 0; k <= 2; k++) {
      expectTrace(q, t, k, false, true);
      expectTrace(q, t, k, true, true);
      expectTrace(q, t, k, false, false);
    }
  }
}

TEST(LevenshteinAutomatonTest, TranspositionIsOneEdit) {
  Automaton swap("abcd", 1, false, true);
  EXPECT_TRUE(swap.a.isMatch(run(swap.a, "abdc")));
  EXPECT_EQ(swap.a.matchDistance(run(swap.a, "bacd")), 1);
  Automaton plain("abcd", 1, false, false);
  EXPECT_FALSE(plain.a.isMatch(run(plain.a, "abdc")));
  // A swap of two multi-byte code points is still one edit.
  std::string e = bytes({0xc3, 0xa9});
  std::string euro = bytes({0xe2, 0x82, 0xac});
  Automaton cp("x" + e + euro, 1, false, true);
  EXPECT_EQ(cp.a.matchDistance(run(cp.a, "x" + euro + e)), 1);
}

TEST(LevenshteinAutomatonTest, CodePointIsOneEdit) {
  // "cafe" vs "caf<e-acute>": one substitution, though the bytes differ by two.
  Automaton a("cafe", 1, false);
  auto s = run(a.a, bytes({'c', 'a', 'f', 0xc3, 0xa9}));
  EXPECT_TRUE(a.a.isMatch(s));
  EXPECT_EQ(a.a.matchDistance(s), 1);
  // Deleting a 4-byte code point is one edit.
  Automaton emoji(bytes({'h', 'i', 0xf0, 0x9f, 0x98, 0x80}), 1, false);
  EXPECT_EQ(emoji.a.matchDistance(run(emoji.a, "hi")), 1);
}

TEST(LevenshteinAutomatonTest, NextLiveByteNeverSkipsAMatch) {
  std::mt19937 rng(0x7154ab1e);
  std::vector<int> starts = {0, 1, 0x7f, 0x80, 0xa0, 0xc3, 0xe3, 0xff};
  for (int i = 0; i < 40; i++) {
    std::string q = randomPieces(rng, 4);
    std::string t = randomPieces(rng, 4);
    for (int k = 0; k <= 2; k++) {
      for (bool prefixMode : {false, true}) {
        Automaton am(q, k, prefixMode);
        const LevenshteinAutomaton& a = am.a;
        auto s = a.start();
        for (int j = 0; j <= (int)t.size(); j++) {
          if (!a.canMatch(s)) break;
          for (int b0 : starts) {
            int next = a.nextLiveByte(s, b0);
            int end = next == LevenshteinAutomaton::DEAD ? 256 : next;
            ASSERT_GE(end, b0);
            for (int b : alphabet()) {
              if (b < b0 || b >= end) continue;
              SCOPED_TRACE("q=" + hex(q) + " t=" + hex(t.substr(0, (size_t)j)) + " b0="
                           + std::to_string(b0) + " b=" + std::to_string(b));
              EXPECT_FALSE(canAccept(a, a.step(s, (uint8_t)b), 3));
            }
          }
          if (j < (int)t.size()) s = a.step(s, byteAt(t, j));
        }
      }
    }
  }
}

TEST(LevenshteinAutomatonTest, SuccessorNeverPassesAMatch) {
  std::mt19937 rng(0x51cc3550);
  for (int i = 0; i < 60; i++) {
    std::string q = randomPieces(rng, 3);
    std::string t = randomPieces(rng, 3);
    if (t.size() > 4) t.resize(4);
    for (int k = 0; k <= 2; k++) {
      expectSuccessorSound(q, k, false, t, 4);
      expectSuccessorSound(q, k, true, t, 4);
    }
  }

  expectSuccessorSound("b", 0, false, "a", 2);
  expectSuccessorSound("", 1, false, "", 1);
  expectSuccessorSound(bytes({0x80}), 0, false, bytes({0x7f}), 1);
  expectSuccessorSound(bytes({0xff}), 0, false, bytes({0xff}), 1);
  expectSuccessorSound("ab", 0, false, bytes({'a', 0xff}), 2);
  expectSuccessorSound("abc", 2, false, "abc", 4);
  expectSuccessorSound(bytes({'a', 0xc3, 0xa9}), 0, false, bytes({'a', 0xc3, 0x80}), 3);
  expectSuccessorSound(bytes({0xe2, 0x82, 0xac}), 0, false, bytes({0xe2, 0x80}), 3);
}

TEST(LevenshteinAutomatonTest, SeeksIntoMultiByteTargets) {
  // From "a" the only live continuation is the e-acute lead, then its tail.
  Automaton am(bytes({'a', 0xc3, 0xa9}), 0, false);
  const LevenshteinAutomaton& a = am.a;
  auto s = a.step(a.start(), 'a');
  EXPECT_EQ(a.nextLiveByte(s, 0), 0xc3);
  EXPECT_EQ(a.nextLiveByte(s, 0xc4), LevenshteinAutomaton::DEAD);
  auto lead = a.step(s, 0xc3);
  EXPECT_EQ(a.nextLiveByte(lead, 0x80), 0xa9);
  EXPECT_EQ(a.nextLiveByte(lead, 0xaa), LevenshteinAutomaton::DEAD);
}

TEST(LevenshteinAutomatonTest, EdgeCases) {
  {
    Automaton am("", 1, false);
    const LevenshteinAutomaton& a = am.a;
    auto s = a.start();
    EXPECT_TRUE(a.isMatch(s));
    EXPECT_EQ(a.matchDistance(s), 0);
    s = a.step(s, 'x');
    EXPECT_TRUE(a.isMatch(s));
    EXPECT_EQ(a.matchDistance(s), 1);
    s = a.step(s, 'y');
    EXPECT_FALSE(a.canMatch(s));
    EXPECT_FALSE(a.isMatch(s));
  }

  {
    Automaton am("ab", 2, true);
    const LevenshteinAutomaton& a = am.a;
    auto s = a.start();
    EXPECT_TRUE(a.isMatch(s));
    EXPECT_EQ(a.matchDistance(s), 2);
    s = a.step(s, 0xff);
    s = a.step(s, 0x00);
    EXPECT_TRUE(a.canMatch(s));
    EXPECT_TRUE(a.isMatch(s));
  }

  {
    Automaton am("ab", 0, false);
    const LevenshteinAutomaton& a = am.a;
    auto s = a.start();
    EXPECT_TRUE(a.canMatch(s));
    EXPECT_FALSE(a.isMatch(s));
    s = a.step(s, 'a');
    EXPECT_TRUE(a.canMatch(s));
    EXPECT_FALSE(a.isMatch(s));
    s = a.step(s, 'b');
    EXPECT_TRUE(a.isMatch(s));
    EXPECT_EQ(a.matchDistance(s), 0);
    s = a.step(s, 'c');
    EXPECT_FALSE(a.canMatch(s));
  }

  {
    Automaton am("ab", 0, true);
    const LevenshteinAutomaton& a = am.a;
    auto s = a.start();
    EXPECT_FALSE(a.isMatch(s));
    s = a.step(s, 'a');
    EXPECT_FALSE(a.isMatch(s));
    s = a.step(s, 'b');
    EXPECT_TRUE(a.isMatch(s));
    EXPECT_EQ(a.matchDistance(s), 0);
    s = a.step(s, 'c');
    EXPECT_TRUE(a.canMatch(s));
    EXPECT_TRUE(a.isMatch(s));
  }

  {
    Automaton am("b", 0, false);
    const LevenshteinAutomaton& a = am.a;
    int liveDepth;
    auto states = liveStack(a, "a", liveDepth);
    using Seek = AutomatonSeekEnum<LevenshteinAutomaton>;
    char buffer[PackedTerm::MAX_LEN + 1];
    int outLen;
    ASSERT_EQ(Seek::successor(a, "a", states.data(), liveDepth, buffer, outLen),
              Seek::Successor::JUMP);
    EXPECT_EQ(std::string(buffer, (size_t)outLen), "b");
  }
}
