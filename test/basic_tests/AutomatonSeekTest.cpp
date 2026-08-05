#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "solux/reader/AutomatonSeekEnum.h"
#include "solux/reader/BruteDfaTermsEnum.h"
#include "solux/util/automaton/WildcardCompiler.h"
#include "solux/util/automaton/RegExpParser.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::automaton;
using namespace solux::test;

namespace {

void addTerms(TestIndex& index, TestField& field, const std::vector<std::string>& terms) {
  field.startIndexing();
  for (size_t i = 0; i < terms.size(); i++) field.add((int)i, terms[i]);
  index.flush();
  field.startReading();
}

std::vector<std::string> collectSmart(TestField& field, const ByteDfa& dfa, int64_t* examined = nullptr,
                                      int64_t* jumps = nullptr) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* segment = field.currentSegment();
  TermsEnum terms(guard.pool(), segment->postingsReader(), field.fieldInfo);
  auto view = dfa.view();
  auto [prefix, state] = view.commonPrefixAndState();
  AutomatonSeekEnum<ByteDfaView> e(guard.pool(), terms, prefix, view, state);
  std::vector<std::string> out;
  while (e.next()) out.emplace_back(e.termView());
  if (examined != nullptr) *examined = e.termsExamined();
  if (jumps != nullptr) *jumps = e.jumps();
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

class AutomatonSeekTest : public SoluxTest {};

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
  for (std::string_view pattern : {"[ab]pp.*", "a{2,3}bc*", "(cat|ban).*"}) {
    Budget budget(10000000);
    ByteDfa dfa = compileRegex(pattern, budget);
    EXPECT_EQ(collectSmart(field, dfa), collectBrute(field, dfa)) << pattern;
  }
}
