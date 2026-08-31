#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/reader/FuzzySeekEnum.h"
#include "luxir/reader/FuzzyTermsEnum.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"

using namespace luxir;
using namespace luxir::test;

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
  float bruteSum = 0.0f;
  float seekSum = 0.0f;
  for (int i = 0; i < (int)brute.size(); i++) {
    EXPECT_EQ(hex(seek[(size_t)i].term), hex(brute[(size_t)i].term)) << "match " << i;
    EXPECT_EQ(std::bit_cast<uint32_t>(seek[(size_t)i].score),
              std::bit_cast<uint32_t>(brute[(size_t)i].score)) << "match " << i;
    seekSum += seek[(size_t)i].score;
    bruteSum += brute[(size_t)i].score;
  }
  EXPECT_EQ(std::bit_cast<uint32_t>(seekSum), std::bit_cast<uint32_t>(bruteSum));
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

class FuzzySeekTest : public LuxirTest {
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

TEST_F(FuzzySeekTest, ScanOracle216Configs) {
  TestIndex ti;
  TestField field(ti, "foo_s");
  indexTerms(ti, field, multiBlockTerms());
  const std::vector<std::string> queries = {
    "", "a", "ab", "apple", "applf", "banana",
    std::string("caf") + bytes({0xc3, 0xa9}),
    bytes({0x80}), bytes({0xff}), bytes({'a', 0xff}),
    repeated('m', PackedTerm::MAX_LEN),
    repeated('q', PackedTerm::MAX_LEN + 3),
  };
  ASSERT_EQ(queries.size() * 3 * 3 * 2, 216);
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

// The seek enum classifies terms in a fused loop and scores only the accepted
// ones through a separate hook, so scoring is the one part of the scan that no
// differential-on-membership test would notice going missing: every score would
// simply stay at its initial 1.0.  These are the exact values.
TEST_F(FuzzySeekTest, ScoresAcceptedTerms) {
  TestIndex ti;
  TestField field(ti, "foo_s");
  std::vector<std::string> terms = {"banana", "beadle", "eedle", "nedle",
                                    "needl", "needle", "needles", "needlf"};
  indexTerms(ti, field, terms);

  // score = 1 - distance / (prefix + min(query suffix, term suffix)).
  std::vector<Match> expected = {
    {"beadle", 1.0f - 2.0f / 6.0f},
    {"eedle", 1.0f - 1.0f / 5.0f},
    {"nedle", 1.0f - 1.0f / 5.0f},
    {"needl", 1.0f - 1.0f / 5.0f},
    {"needle", 1.0f},
    {"needles", 1.0f - 1.0f / 6.0f},
    {"needlf", 1.0f - 1.0f / 6.0f},
  };
  auto seek = collectSeek(field, "", "needle", 2, false);
  ASSERT_EQ(seek.size(), expected.size());
  for (int i = 0; i < (int)expected.size(); i++) {
    EXPECT_EQ(seek[(size_t)i].term, expected[(size_t)i].term) << "match " << i;
    EXPECT_FLOAT_EQ(seek[(size_t)i].score, expected[(size_t)i].score) << "match " << i;
  }

  // Prefix mode scores the closest prefix of the term over a fixed denominator.
  std::vector<Match> expectedPrefix = {
    {"eedle", 1.0f - 1.0f / 4.0f},
    {"nedle", 1.0f - 1.0f / 4.0f},
    {"needl", 1.0f},
    {"needle", 1.0f},
    {"needles", 1.0f},
    {"needlf", 1.0f},
  };
  auto seekPrefix = collectSeek(field, "", "need", 1, true);
  ASSERT_EQ(seekPrefix.size(), expectedPrefix.size());
  for (int i = 0; i < (int)expectedPrefix.size(); i++) {
    EXPECT_EQ(seekPrefix[(size_t)i].term, expectedPrefix[(size_t)i].term) << "prefix " << i;
    EXPECT_FLOAT_EQ(seekPrefix[(size_t)i].score, expectedPrefix[(size_t)i].score)
        << "prefix " << i;
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
