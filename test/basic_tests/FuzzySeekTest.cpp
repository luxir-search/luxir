#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "solux/reader/FuzzySeekEnum.h"
#include "solux/reader/FuzzyTermsEnum.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

namespace {

struct Match {
  std::string term;
  float score;
};

std::string bytes(std::initializer_list<int> values) {
  std::string s;
  for (int v : values) s.push_back((char)(uint8_t)v);
  return s;
}

std::string repeated(int b, int len) {
  return std::string((size_t)len, (char)(uint8_t)b);
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

void addUnique(std::vector<std::string>& terms, std::string term) {
  ASSERT_LE(term.size(), PackedTerm::MAX_LEN);
  if (std::find(terms.begin(), terms.end(), term) == terms.end()) {
    terms.push_back(std::move(term));
  }
}

std::string randomTerm(std::mt19937& rng, int maxLen, bool rawBytes = true) {
  std::vector<int> alphabet = {'a', 'b', 'c', 'd', 'e', 'p', 'r', 's'};
  std::uniform_int_distribution<int> lenDist(1, maxLen);
  std::uniform_int_distribution<int> choiceDist(0, (int)alphabet.size() - 1);
  std::uniform_int_distribution<int> byteDist(0, 255);
  std::uniform_int_distribution<int> rawDist(0, 6);
  int len = lenDist(rng);
  std::string s;
  for (int i = 0; i < len; i++) {
    int b = rawBytes && rawDist(rng) == 0 ? byteDist(rng) : alphabet[(size_t)choiceDist(rng)];
    s.push_back((char)(uint8_t)b);
  }
  return s;
}

std::vector<std::string> multiBlockTerms() {
  std::vector<std::string> terms;
  for (std::string_view s : {"a", "ab", "abc", "app", "apple", "apples",
                             "application", "banana", "band", "bandana",
                             "car", "cart", "dog"}) {
    addUnique(terms, std::string(s));
  }
  addUnique(terms, std::string("caf") + bytes({0xc3, 0xa9}));
  addUnique(terms, std::string("na") + bytes({0xc3, 0xaf}) + "ve");
  addUnique(terms, bytes({0x80, 'r', 'a', 'w'}));
  addUnique(terms, bytes({0xff, 'r', 'a', 'w'}));
  addUnique(terms, bytes({'a', 0xff, 'z'}));
  addUnique(terms, repeated('m', PackedTerm::MAX_LEN));
  addUnique(terms, repeated('m', PackedTerm::MAX_LEN - 1) + "n");
  addUnique(terms, repeated('z', PackedTerm::MAX_LEN));

  std::string sharedPrefix(140, 'u');
  for (int i = 0; i < 40; i++) {
    std::string suffix;
    suffix.push_back((char)('A' + i));
    suffix.push_back((char)('a' + (i % 26)));
    addUnique(terms, sharedPrefix + suffix);
  }

  std::string longSuffixPrefix(16, 'v');
  for (int i = 0; i < 20; i++) {
    std::string t = longSuffixPrefix;
    t.push_back((char)('a' + i));
    t += std::string(180, (char)('k' + (i % 3)));
    addUnique(terms, std::move(t));
  }

  std::mt19937 rng(0xf0221e55);
  std::vector<std::string> clusters = {"app", "ban", "car", "pre"};
  std::uniform_int_distribution<int> clusterDist(0, (int)clusters.size() - 1);
  while ((int)terms.size() < 320) {
    std::string t = clusters[(size_t)clusterDist(rng)];
    t += randomTerm(rng, 8, true);
    if (t.size() > PackedTerm::MAX_LEN) t.resize(PackedTerm::MAX_LEN);
    addUnique(terms, std::move(t));
  }
  return terms;
}

void indexTerms(TestIndex& ti, TestField& field, const std::vector<std::string>& terms) {
  field.startIndexing();
  for (int i = 0; i < (int)terms.size(); i++) {
    field.add(i, terms[(size_t)i]);
  }
  ti.flush();
  field.startReading();
  ASSERT_NE(field.currentSegment(), nullptr);
}

std::vector<Match> collectBrute(TestField& field, std::string_view prefix,
                                std::string_view suffix, int k, bool prefixMode) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  FuzzyTermsEnum e(guard.pool(), te, prefix, suffix, k, prefixMode);
  std::vector<Match> out;
  while (e.next()) {
    std::string_view t = e.termView();
    out.push_back({std::string(t), e.currentScore()});
  }
  return out;
}

std::vector<Match> collectSeek(TestField& field, std::string_view prefix,
                               std::string_view suffix, int k, bool prefixMode,
                               int64_t* termsExamined = nullptr,
                               int64_t* jumps = nullptr) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  FuzzySeekEnum e(guard.pool(), te, prefix, suffix, k, prefixMode);
  std::vector<Match> out;
  while (e.next()) {
    std::string_view t = e.termView();
    out.push_back({std::string(t), e.currentScore()});
  }
  if (termsExamined != nullptr) *termsExamined = e.termsExamined();
  if (jumps != nullptr) *jumps = e.jumps();
  return out;
}

void expectSame(const std::vector<Match>& brute, const std::vector<Match>& seek,
                std::string_view query, int prefixLen, int k, bool prefixMode) {
  SCOPED_TRACE("q=" + hex(query) + " prefixLen=" + std::to_string(prefixLen)
               + " k=" + std::to_string(k) + " prefixMode=" + std::to_string((int)prefixMode));
  ASSERT_EQ(seek.size(), brute.size());
  for (int i = 0; i < (int)brute.size(); i++) {
    EXPECT_EQ(hex(seek[(size_t)i].term), hex(brute[(size_t)i].term)) << "match " << i;
    EXPECT_FLOAT_EQ(seek[(size_t)i].score, brute[(size_t)i].score) << "match " << i;
  }
}

void expectEnumsEqual(TestField& field, std::string_view query, int prefixLen, int k,
                      bool prefixMode) {
  int pl = std::min(prefixLen, (int)query.size());
  std::string_view prefix = query.substr(0, (size_t)pl);
  std::string_view suffix = query.substr((size_t)pl);
  auto brute = collectBrute(field, prefix, suffix, k, prefixMode);
  auto seek = collectSeek(field, prefix, suffix, k, prefixMode);
  expectSame(brute, seek, query, prefixLen, k, prefixMode);
}

void expectCrossProduct(TestField& field, const std::vector<std::string>& queries) {
  for (const auto& q : queries) {
    for (int prefixLen : {0, 1, 2}) {
      for (int k = 0; k <= 2; k++) {
        expectEnumsEqual(field, q, prefixLen, k, false);
        expectEnumsEqual(field, q, prefixLen, k, true);
      }
    }
  }
}

bool containsTerm(const std::vector<Match>& matches, std::string_view term) {
  return std::find_if(matches.begin(), matches.end(), [&](const Match& m) {
    return m.term == term;
  }) != matches.end();
}

} // namespace

class FuzzySeekTest : public SoluxTest {
};

TEST_F(FuzzySeekTest, DifferentialSingleTermAndOneBlock) {
  {
    TestIndex ti;
    TestField field(ti, "foo_s");
    std::vector<std::string> terms = {"apple"};
    indexTerms(ti, field, terms);
    expectCrossProduct(field, {"apple", "apples", "zz"});
  }

  {
    TestIndex ti;
    TestField field(ti, "foo_s");
    std::vector<std::string> terms = {
      "a", "ab", "abc", "apple", "apples", "apply", "banana",
      bytes({0x80}), bytes({0xff}), bytes({'a', 0xff})
    };
    indexTerms(ti, field, terms);
    expectCrossProduct(field, {"", "apple", "applf", "zz", bytes({0x80}), bytes({0xff})});
  }

  {
    TestIndex ti;
    TestField field(ti, "foo_s");
    std::vector<std::string> terms = {repeated('m', PackedTerm::MAX_LEN)};
    indexTerms(ti, field, terms);
    expectCrossProduct(field, {repeated('m', PackedTerm::MAX_LEN),
                               repeated('q', PackedTerm::MAX_LEN + 3)});
  }
}

TEST_F(FuzzySeekTest, DifferentialRandomMultiBlock) {
  TestIndex ti;
  TestField field(ti, "foo_s");
  std::vector<std::string> terms = multiBlockTerms();
  ASSERT_GT(terms.size(), 200);
  indexTerms(ti, field, terms);

  std::vector<std::string> queries = {
    "", "a", "ab", "apple", "applf", "banana", "car",
    std::string("caf") + bytes({0xc3, 0xa9}),
    bytes({0x80}), bytes({0xff}), bytes({'a', 0xff}),
    repeated('m', PackedTerm::MAX_LEN),
    repeated('q', PackedTerm::MAX_LEN + 3),
  };

  std::mt19937 rng(0x5eed51f7);
  for (int i = 0; i < 10; i++) queries.push_back(randomTerm(rng, 8));
  expectCrossProduct(field, queries);
}

TEST_F(FuzzySeekTest, EdgeAssertions) {
  TestIndex ti;
  TestField field(ti, "foo_s");
  std::vector<std::string> terms = multiBlockTerms();
  indexTerms(ti, field, terms);

  {
    std::string query = "apple";
    std::string_view prefix = query;
    std::string_view suffix;
    auto seek = collectSeek(field, prefix, suffix, 1, false);
    auto brute = collectBrute(field, prefix, suffix, 1, false);
    expectSame(brute, seek, query, 5, 1, false);
    EXPECT_TRUE(containsTerm(seek, "apple"));
    EXPECT_TRUE(containsTerm(seek, "apples"));
  }

  {
    std::string query = "ab";
    auto seek = collectSeek(field, "", query, 2, true);
    auto brute = collectBrute(field, "", query, 2, true);
    expectSame(brute, seek, query, 0, 2, true);
    EXPECT_EQ(seek.size(), terms.size());
  }

  {
    std::string query = "zz";
    auto seek = collectSeek(field, "zz", "", 2, false);
    auto brute = collectBrute(field, "zz", "", 2, false);
    expectSame(brute, seek, query, 2, 2, false);
    EXPECT_TRUE(seek.empty());
  }

  {
    std::string query = repeated('q', PackedTerm::MAX_LEN + 3);
    auto seek = collectSeek(field, "", query, 2, false);
    auto brute = collectBrute(field, "", query, 2, false);
    expectSame(brute, seek, query, 0, 2, false);
    EXPECT_TRUE(seek.empty());
  }

  {
    int64_t termsExamined = 0;
    std::string query = "zzzz";
    auto seek = collectSeek(field, "", query, 0, false, &termsExamined);
    auto brute = collectBrute(field, "", query, 0, false);
    expectSame(brute, seek, query, 0, 0, false);
    EXPECT_TRUE(seek.empty());
    EXPECT_LT(termsExamined, (int64_t)terms.size());
  }
}

TEST_F(FuzzySeekTest, JumpEffectivenessSparseCorpus) {
  TestIndex ti;
  TestField field(ti, "foo_s");
  std::vector<std::string> terms;
  for (int i = 0; i < 1200; i++) {
    std::string t;
    t.push_back((char)('a' + (i % 26)));
    t += "zz";
    t += std::to_string(100000 + i);
    t += std::string((size_t)(i % 7), 'q');
    addUnique(terms, std::move(t));
  }
  for (std::string_view s : {"neadle", "needle", "needlf", "needles"}) {
    addUnique(terms, std::string(s));
  }
  indexTerms(ti, field, terms);

  int64_t termsExamined = 0;
  int64_t jumps = 0;
  std::string query = "needle";
  auto seek = collectSeek(field, "", query, 1, false, &termsExamined, &jumps);
  auto brute = collectBrute(field, "", query, 1, false);
  expectSame(brute, seek, query, 0, 1, false);
  ASSERT_FALSE(seek.empty());
  EXPECT_LT(termsExamined, (int64_t)terms.size() / 2);
  EXPECT_GT(jumps, 0);
}
