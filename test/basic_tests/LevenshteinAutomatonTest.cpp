#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <random>
#include <string>
#include <string.h>
#include <string_view>
#include <vector>

#include "solux/reader/LevenshteinAutomaton.h"
#include "solux/reader/AutomatonSeekEnum.h"

using namespace solux;

namespace {

struct RefState {
  int j;
  std::vector<int> row;
  bool prefixMatched;
  int bestPrefixDist;
};

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

std::string randomBytes(std::mt19937& rng, int maxLen) {
  std::uniform_int_distribution<int> lenDist(0, maxLen);
  std::uniform_int_distribution<int> byteDist(0, 255);
  std::string s;
  int len = lenDist(rng);
  for (int i = 0; i < len; i++) s.push_back((char)(uint8_t)byteDist(rng));
  return s;
}

std::string randomAlphabetBytes(std::mt19937& rng, int maxLen) {
  std::vector<int> alphabet = {0x00, 0x01, 'a', 'b', 'c', 0x7f, 0x80, 0xfe, 0xff};
  std::uniform_int_distribution<int> lenDist(0, maxLen);
  std::uniform_int_distribution<int> choiceDist(0, (int)alphabet.size() - 1);
  std::uniform_int_distribution<int> byteDist(0, 255);
  std::uniform_int_distribution<int> rawDist(0, 5);
  std::string s;
  int len = lenDist(rng);
  for (int i = 0; i < len; i++) {
    int b = rawDist(rng) == 0 ? byteDist(rng) : alphabet[(size_t)choiceDist(rng)];
    s.push_back((char)(uint8_t)b);
  }
  return s;
}

RefState refStart(std::string_view query, int k, bool prefixMode) {
  RefState s;
  s.j = 0;
  s.row.resize(query.size() + 1);
  for (int i = 0; i <= (int)query.size(); i++) s.row[(size_t)i] = i;
  s.prefixMatched = prefixMode && s.row[query.size()] <= k;
  s.bestPrefixDist = prefixMode ? std::min(k + 1, s.row[query.size()]) : k + 1;
  return s;
}

RefState refStep(std::string_view query, int k, bool prefixMode, const RefState& s, uint8_t b) {
  int n = (int)query.size();
  RefState d;
  d.j = s.j + 1;
  d.row.assign((size_t)n + 1, 0);
  d.row[0] = d.j;
  for (int i = 1; i <= n; i++) {
    int sub = s.row[(size_t)i - 1] + (b == byteAt(query, i - 1) ? 0 : 1);
    int ins = s.row[(size_t)i] + 1;
    int del = d.row[(size_t)i - 1] + 1;
    d.row[(size_t)i] = std::min({sub, ins, del});
  }
  if (prefixMode) {
    d.prefixMatched = s.prefixMatched || d.row[(size_t)n] <= k;
    d.bestPrefixDist = std::min(s.bestPrefixDist, std::min(k + 1, d.row[(size_t)n]));
  } else {
    d.prefixMatched = false;
    d.bestPrefixDist = k + 1;
  }
  return d;
}

bool refCanMatch(const RefState& s, int k, bool prefixMode) {
  if (prefixMode && s.prefixMatched) return true;
  return *std::min_element(s.row.begin(), s.row.end()) <= k;
}

bool refIsMatch(const RefState& s, int k, bool prefixMode) {
  return prefixMode ? s.prefixMatched : s.row.back() <= k;
}

int refMatchDistance(const RefState& s, bool prefixMode) {
  return prefixMode ? s.bestPrefixDist : s.row.back();
}

void expectState(const LevenshteinAutomaton& a, const LevenshteinAutomaton::State& s,
                 const RefState& r, int k, bool prefixMode) {
  EXPECT_EQ(a.canMatch(s), refCanMatch(r, k, prefixMode));
  EXPECT_EQ(a.isMatch(s), refIsMatch(r, k, prefixMode));
  if (a.isMatch(s) && refIsMatch(r, k, prefixMode)) {
    EXPECT_EQ(a.matchDistance(s), refMatchDistance(r, prefixMode));
  }
}

void expectTrace(std::string_view query, std::string_view term, int k, bool prefixMode) {
  LevenshteinAutomaton a(query, k, prefixMode);
  auto s = a.start();
  RefState r = refStart(query, k, prefixMode);
  {
    SCOPED_TRACE("q=" + hex(query) + " t=" + hex(term) + " start");
    expectState(a, s, r, k, prefixMode);
  }
  for (int j = 0; j < (int)term.size(); j++) {
    s = a.step(s, byteAt(term, j));
    r = refStep(query, k, prefixMode, r, byteAt(term, j));
    SCOPED_TRACE("q=" + hex(query) + " t=" + hex(term) + " j=" + std::to_string(j + 1));
    expectState(a, s, r, k, prefixMode);
  }
}

int trialNextLiveByte(const LevenshteinAutomaton& a, const LevenshteinAutomaton::State& s,
                      int b0) {
  for (int b = b0; b <= 255; b++) {
    if (a.canMatch(a.step(s, (uint8_t)b))) return b;
  }
  return LevenshteinAutomaton::DEAD;
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

int compareBytes(std::string_view a, std::string_view b) {
  int len = std::min((int)a.size(), (int)b.size());
  int cmp = memcmp(a.data(), b.data(), (size_t)len);
  return cmp != 0 ? cmp : (int)a.size() - (int)b.size();
}

bool isLive(const LevenshteinAutomaton& a, std::string_view term) {
  auto s = a.start();
  for (int i = 0; i < (int)term.size(); i++) s = a.step(s, byteAt(term, i));
  return a.canMatch(s);
}

bool subtreeCanExceed(std::string_view cur, std::string_view target, int maxLen) {
  std::string maxString(cur);
  maxString.append((size_t)(maxLen - (int)cur.size()), (char)0xff);
  return compareBytes(maxString, target) > 0;
}

bool bruteSuccessorDfs(const LevenshteinAutomaton& a, std::string_view target, int maxLen,
                       std::string& cur, std::string& out) {
  if (!subtreeCanExceed(cur, target, maxLen)) return false;
  if (!isLive(a, cur)) return false;
  if (compareBytes(cur, target) > 0) {
    out = cur;
    return true;
  }
  if ((int)cur.size() == maxLen) return false;
  for (int b = 0; b <= 255; b++) {
    cur.push_back((char)(uint8_t)b);
    if (bruteSuccessorDfs(a, target, maxLen, cur, out)) return true;
    cur.pop_back();
  }
  return false;
}

bool bruteSuccessor(const LevenshteinAutomaton& a, std::string_view target, int maxLen,
                    std::string& out) {
  std::string cur;
  out.clear();
  return bruteSuccessorDfs(a, target, maxLen, cur, out);
}

void expectSuccessor(std::string_view query, int k, bool prefixMode, std::string_view term,
                     int maxLen) {
  LevenshteinAutomaton a(query, k, prefixMode);
  int liveDepth;
  auto states = liveStack(a, term, liveDepth);
  std::string actual;
  char buffer[PackedTerm::MAX_LEN + 1];
  int actualLen;
  bool actualFound = AutomatonSeekEnum<LevenshteinAutomaton>::successor(
      a, term, states.data(), liveDepth, buffer, actualLen);
  actual.assign(buffer, (size_t)actualLen);
  std::string expected;
  bool expectedFound = bruteSuccessor(a, term, maxLen, expected);

  SCOPED_TRACE("q=" + hex(query) + " t=" + hex(term) + " k=" + std::to_string(k));
  EXPECT_EQ(actualFound, expectedFound);
  if (actualFound && expectedFound) {
    EXPECT_EQ(hex(actual), hex(expected));
  }
}

} // namespace

TEST(LevenshteinAutomatonTest, RowsMatchFullDpReference) {
  std::vector<std::string> queries = {
    "",
    "a",
    "ab",
    bytes({0x80, 0xff, 'b'}),
  };
  std::vector<std::string> terms = {
    "",
    "a",
    "abc",
    bytes({0x80, 0xff}),
    bytes({0xff, 0x00}),
  };

  for (const auto& q : queries) {
    for (const auto& t : terms) {
      for (int k = 0; k <= 2; k++) {
        expectTrace(q, t, k, false);
        expectTrace(q, t, k, true);
      }
    }
  }

  std::mt19937 rng(0x5eed1234);
  for (int i = 0; i < 420; i++) {
    std::string q = randomBytes(rng, 8);
    std::string t = randomBytes(rng, 10);
    for (int k = 0; k <= 2; k++) {
      expectTrace(q, t, k, false);
      expectTrace(q, t, k, true);
    }
  }
}

TEST(LevenshteinAutomatonTest, NextLiveByteMatchesTrialStepping) {
  std::mt19937 rng(0x7154ab1e);
  std::vector<int> starts = {0, 1, 0x7f, 0x80, 0xfe, 0xff};

  for (int i = 0; i < 240; i++) {
    std::string q = randomBytes(rng, 10);
    std::string t = randomBytes(rng, 10);
    for (int k = 0; k <= 2; k++) {
      for (bool prefixMode : {false, true}) {
        LevenshteinAutomaton a(q, k, prefixMode);
        auto s = a.start();
        for (int j = 0; j <= (int)t.size(); j++) {
          if (a.canMatch(s)) {
            for (int b0 : starts) {
              SCOPED_TRACE("q=" + hex(q) + " t=" + hex(t) + " j=" + std::to_string(j));
              EXPECT_EQ(a.nextLiveByte(s, b0), trialNextLiveByte(a, s, b0));
            }
          }
          if (j < (int)t.size()) s = a.step(s, byteAt(t, j));
        }
      }
    }
  }
}

TEST(LevenshteinAutomatonTest, SuccessorMatchesBruteEnumeration) {
  std::mt19937 rng(0x51cc3550);
  for (int i = 0; i < 150; i++) {
    std::string q = randomAlphabetBytes(rng, 4);
    std::string t = randomAlphabetBytes(rng, 4);
    for (int k = 0; k <= 2; k++) {
      expectSuccessor(q, k, false, t, 5);
      expectSuccessor(q, k, true, t, 5);
    }
  }

  expectSuccessor("b", 0, false, "a", 2);
  expectSuccessor("", 1, false, "", 1);
  expectSuccessor(bytes({0x80}), 0, false, bytes({0x7f}), 1);
  expectSuccessor(bytes({0xff}), 0, false, bytes({0xff}), 1);
  expectSuccessor("ab", 0, false, bytes({'a', 0xff}), 2);
  expectSuccessor("abc", 2, false, "abc", 5);
  expectSuccessor("abcx", 0, false, "abdz", 5);
}

TEST(LevenshteinAutomatonTest, EdgeCases) {
  {
    LevenshteinAutomaton a("", 1, false);
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
    LevenshteinAutomaton a("ab", 2, true);
    auto s = a.start();
    EXPECT_TRUE(a.isMatch(s));
    EXPECT_EQ(a.matchDistance(s), 2);
    s = a.step(s, 0xff);
    s = a.step(s, 0x00);
    EXPECT_TRUE(a.canMatch(s));
    EXPECT_TRUE(a.isMatch(s));
  }

  {
    LevenshteinAutomaton a("ab", 0, false);
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
    LevenshteinAutomaton a("ab", 0, true);
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
    LevenshteinAutomaton a("b", 0, false);
    int liveDepth;
    auto states = liveStack(a, "a", liveDepth);
    std::string out;
    char buffer[PackedTerm::MAX_LEN + 1];
    int outLen;
    ASSERT_TRUE(AutomatonSeekEnum<LevenshteinAutomaton>::successor(
        a, "a", states.data(), liveDepth, buffer, outLen));
    out.assign(buffer, (size_t)outLen);
    EXPECT_EQ(out, "b");
  }
}
