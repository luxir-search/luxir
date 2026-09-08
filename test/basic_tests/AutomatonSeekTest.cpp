// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "luxir/reader/AutomatonSeekEnum.h"
#include "luxir/reader/BruteDfaTermsEnum.h"
#include "luxir/reader/DfaIntersectEnum.h"
#include "luxir/util/automaton/WildcardCompiler.h"
#include "luxir/util/automaton/RegExpParser.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"

using namespace luxir;
using namespace luxir::automaton;
using namespace luxir::test;

namespace {

struct DfaStats {
  int64_t examined = 0;
  int64_t steps = 0;
  int64_t jumps = 0;
  int64_t successorPlans = 0;
  int64_t targetComparisons = 0;
  int64_t linearTerms = 0;
  int64_t suffixRejects = 0;
  int64_t dfaTerms = 0;
};

void addTerms(TestIndex& index, TestField& field, const std::vector<std::string>& terms) {
  field.startIndexing();
  for (size_t i = 0; i < terms.size(); i++) field.add((int)i, terms[i]);
  index.flush();
  field.startReading();
}

std::vector<std::string> collectSmart(TestField& field, const ByteDfa& dfa, int64_t* examined = nullptr,
                                      int64_t* jumps = nullptr, DfaStats* stats = nullptr) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* segment = field.currentSegment();
  TermsEnum terms(guard.pool(), segment->postingsReader(), field.fieldInfo);
  auto view = dfa.view();
  auto [prefix, state] = view.commonPrefixAndState();
  std::string commonSuffix = dfa.commonSuffix();
  DfaScanPlan plan{prefix, commonSuffix, state};
  DfaIntersectEnum e(guard.pool(), terms, view, plan);
  std::vector<std::string> out;
  while (e.next()) out.emplace_back(e.termView());
  if (examined != nullptr) *examined = e.termsExamined();
  if (jumps != nullptr) *jumps = e.jumps();
  if (stats != nullptr) {
    *stats = {e.termsExamined(), e.dpSteps(), e.jumps(), e.successorPlans(),
              e.targetComparisons(), e.linearTerms(), e.suffixRejects(),
              e.dfaTerms()};
  }
  return out;
}

std::vector<std::string> collectLegacy(TestField& field, const ByteDfa& dfa,
                                       DfaStats& stats) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* segment = field.currentSegment();
  TermsEnum terms(guard.pool(), segment->postingsReader(), field.fieldInfo);
  auto view = dfa.view();
  auto [prefix, state] = view.commonPrefixAndState();
  AutomatonSeekEnum<ByteDfaView> e(guard.pool(), terms, prefix, view, state);
  std::vector<std::string> out;
  while (e.next()) out.emplace_back(e.termView());
  stats.examined = e.termsExamined();
  stats.steps = e.dpSteps();
  stats.jumps = e.jumps();
  return out;
}

std::vector<std::string> collectBrute(TestField& field, const ByteDfa& dfa) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* segment = field.currentSegment();
  TermsEnum terms(guard.pool(), segment->postingsReader(), field.fieldInfo);
  BruteDfaTermsEnum e(terms, dfa.view());
  std::vector<std::string> out;
  while (e.next()) out.emplace_back(e.termView());
  return out;
}

std::vector<std::string> corpus(std::mt19937& rng) {
  std::vector<std::string> terms = {"apple", "application", "banana", "caf\xc3\xa9", "na\xc3\xafve"};
  const std::vector<std::string> clusters = {"app", "ban", "cat", "pre"};
  while (terms.size() < 260) {
    std::string term = clusters[(size_t)(rng() % clusters.size())];
    int length = 1 + (int)(rng() % 8);
    for (int i = 0; i < length; i++) {
      unsigned char byte = rng() % 7 == 0 ? (unsigned char)(0x80 + rng() % 128)
          : (unsigned char)('a' + rng() % 6);
      term.push_back((char)byte);
    }
    if (std::find(terms.begin(), terms.end(), term) == terms.end()) terms.push_back(std::move(term));
  }
  return terms;
}

std::string pattern(std::mt19937& rng) {
  const std::string alphabet = "abc";
  std::string result;
  for (int i = 0, count = 2 + (int)(rng() % 5); i < count; i++) {
    int choice = rng() % 6;
    if (choice == 0) result += '*';
    else if (choice == 1) result += '?';
    else if (choice == 2) result += "\\*";
    else result += alphabet[(size_t)(rng() % alphabet.size())];
  }
  return result;
}

} // namespace

class AutomatonSeekTest : public LuxirTest {};

TEST_F(AutomatonSeekTest, WildcardDifferentialAndLowerBound) {
  std::mt19937 rng(0x4a9c31);
  TestIndex index;
  TestField field(index, "value_s");
  addTerms(index, field, corpus(rng));
  for (int i = 0; i < 80; i++) {
    Budget budget(10000000);
    std::string wildcard = pattern(rng);
    ByteDfa dfa = compileWildcard(wildcard, budget);
    // Equality with the full-run brute oracle proves every smart seek gap has
    // no accepted term: any accepted term below the next landed term would be
    // present in the brute sequence at a missing position.
    EXPECT_EQ(collectSmart(field, dfa), collectBrute(field, dfa)) << wildcard;
  }
}

TEST_F(AutomatonSeekTest, SparseWildcardJumps) {
  TestIndex index;
  TestField field(index, "value_s");
  std::vector<std::string> terms;
  for (int i = 0; i < 500; i++) terms.push_back("noise" + std::to_string(i));
  terms.push_back("match_a_end");
  terms.push_back("match_b_end");
  terms.push_back("match_c_end");
  addTerms(index, field, terms);
  Budget budget(10000000);
  ByteDfa dfa = compileWildcard("match_*_end", budget);
  int64_t examined;
  auto smart = collectSmart(field, dfa, &examined);
  EXPECT_EQ(smart, collectBrute(field, dfa));
  EXPECT_EQ(smart.size(), 3);
  EXPECT_LT(examined, 30);
}

TEST_F(AutomatonSeekTest, RegexDifferential) {
  std::mt19937 rng(0x4a9c31);
  TestIndex index;
  TestField field(index, "value_s");
  addTerms(index, field, corpus(rng));
  for (std::string_view pattern : {"[ab]pp.*", "a{2,3}bc*", "(cat|ban).*",
                                   ".*a", "[ab]*c", "(app|ban).*a"}) {
    Budget budget(10000000);
    ByteDfa dfa = compileRegex(pattern, budget);
    EXPECT_EQ(collectSmart(field, dfa), collectBrute(field, dfa)) << pattern;
  }
}

TEST_F(AutomatonSeekTest, ScanOraclePatternCorpus) {
  struct Pattern {
    std::string_view text;
    bool regex;
  };
  const std::vector<Pattern> patterns = {
    {"*ology", false}, {"*sband", false}, {"comp*ing", false},
    {"*graph*", false}, {"match_*_end", false}, {"a?b", false},
    {"*tail", false}, {"prefix*end", false},
    {".*ization", true}, {"[jkqxz][a-z]*ess", true},
    {"[a-f0-9]{8,}", true}, {"(inter|under)[a-z]*", true},
    {"(re|un)[a-z]*ing", true}, {"[a-z]*ph[oi]lic", true},
    {"[a-z]+ization", true}, {"(app|ban).*a", true},
    {"[bcd]o[aeiou]t", true},
  };
  ASSERT_EQ(patterns.size(), 17);

  std::mt19937 rng(0x4a9c31);
  TestIndex index;
  TestField field(index, "value_s");
  addTerms(index, field, corpus(rng));
  for (const Pattern& pattern : patterns) {
    Budget budget(10000000);
    ByteDfa dfa = pattern.regex ? compileRegex(pattern.text, budget)
                                : compileWildcard(pattern.text, budget);
    EXPECT_EQ(collectSmart(field, dfa), collectBrute(field, dfa)) << pattern.text;
  }
}

TEST_F(AutomatonSeekTest, LeadingSuffixUsesLinearRanges) {
  TestIndex index;
  TestField field(index, "value_s");
  std::vector<std::string> terms;
  for (int i = 0; i < 600; i++) {
    std::string term = "term" + std::to_string(i);
    term += i % 37 == 0 ? "ology" : "other";
    terms.push_back(std::move(term));
  }
  addTerms(index, field, terms);
  Budget budget(10000000);
  ByteDfa dfa = compileWildcard("*ology", budget);
  DfaStats stats;
  EXPECT_EQ(collectSmart(field, dfa, nullptr, nullptr, &stats), collectBrute(field, dfa));
  EXPECT_GT(stats.linearTerms, stats.examined * 10);
  EXPECT_GT(stats.suffixRejects, 500);
  EXPECT_EQ(stats.dfaTerms,
            stats.examined + stats.linearTerms - stats.suffixRejects);
}

TEST_F(AutomatonSeekTest, SuffixEmptyPreservesSuccessorOracle) {
  std::mt19937 rng(0x99da27);
  TestIndex index;
  TestField field(index, "value_s");
  addTerms(index, field, corpus(rng));

  for (std::string_view pattern : {"[a-f0-9]{8,}", "(inter|under)[a-z]*",
                                   "[bcd]o[aeiou]t"}) {
    Budget budget(10000000);
    ByteDfa dfa = compileRegex(pattern, budget);
    ASSERT_TRUE(dfa.commonSuffix().empty()) << pattern;
    DfaStats legacy;
    DfaStats current;
    auto expected = collectLegacy(field, dfa, legacy);
    auto actual = collectSmart(field, dfa, nullptr, nullptr, &current);
    EXPECT_EQ(actual, expected) << pattern;
    EXPECT_EQ(current.examined, legacy.examined) << pattern;
    EXPECT_EQ(current.steps, legacy.steps) << pattern;
    EXPECT_EQ(current.jumps, legacy.jumps) << pattern;
    EXPECT_EQ(current.linearTerms, 0) << pattern;
    EXPECT_EQ(current.suffixRejects, 0) << pattern;
    EXPECT_EQ(current.dfaTerms, current.examined) << pattern;
  }

  Budget budget(10000000);
  ByteDfa dfa = compileWildcard("*graph*", budget);
  ASSERT_TRUE(dfa.commonSuffix().empty());
  ASSERT_TRUE(dfa.view().allStatesLiveOnAllBytes());
  DfaStats legacy;
  DfaStats current;
  EXPECT_EQ(collectSmart(field, dfa, nullptr, nullptr, &current),
            collectLegacy(field, dfa, legacy));
  EXPECT_EQ(current.examined, legacy.examined);
  EXPECT_EQ(current.steps, legacy.steps);
  EXPECT_EQ(current.jumps, legacy.jumps);
  EXPECT_EQ(current.targetComparisons, 0);
  EXPECT_EQ(current.linearTerms, 0);
  EXPECT_EQ(current.dfaTerms, current.examined);
}

TEST_F(AutomatonSeekTest, SuffixDifferentialAcrossDictionaryEdges) {
  TestIndex index;
  TestField field(index, "value_s");
  std::vector<std::string> terms;
  terms.emplace_back("\0a_tail", 7);
  terms.emplace_back("\0b_other", 8);
  for (int i = 0; i < 100; i++) {
    std::string term(80, (char)('a' + i % 4));
    term += std::to_string(i);
    term += i % 9 == 0 ? "tail" : "nope";
    terms.push_back(std::move(term));
  }
  terms.push_back(std::string(PackedTerm::MAX_LEN - 4, 'm') + "tail");
  terms.push_back(std::string(PackedTerm::MAX_LEN - 4, (char)0xff) + "tail");
  terms.push_back(std::string(1, (char)0xff) + "other");
  addTerms(index, field, terms);

  for (std::string_view wildcard : {"*tail", "*other"}) {
    Budget budget(10000000);
    ByteDfa dfa = compileWildcard(wildcard, budget);
    EXPECT_EQ(collectSmart(field, dfa), collectBrute(field, dfa)) << wildcard;
  }
}
