#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "test/SoluxTest.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

namespace {

void indexTerms(TestIndex& ti, TestField& field, const std::vector<std::string>& terms) {
  field.startIndexing();
  for (int i = 0; i < (int)terms.size(); i++) {
    field.add(i, terms[(size_t)i]);
  }
  ti.flush();
  field.startReading();
  ASSERT_NE(field.currentSegment(), nullptr);
}

std::vector<std::string> collectNext(TestField& field) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  std::vector<std::string> got;
  while (te.nextTerm()) {
    got.push_back(std::string((std::string_view)te.term()));
  }
  return got;
}

std::vector<std::string> collectSeekCeil(TestField& field) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  std::vector<std::string> got;
  std::string target;
  while (te.seekCeil(target)) {
    std::string term((std::string_view)te.term());
    got.push_back(term);
    target = term;
    target.push_back('\0');
  }
  return got;
}

void expectExactSeeks(TestField& field, const std::vector<std::string>& expected) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  for (int i = (int)expected.size() - 1; i >= 0; i--) {
    ASSERT_TRUE(te.seek(expected[(size_t)i])) << i;
    EXPECT_EQ((std::string_view)te.term(), expected[(size_t)i]) << i;
  }
}

void expectCeil(TestField& field, std::string_view target, std::string_view expected) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  ASSERT_TRUE(te.seekCeil(target));
  EXPECT_EQ((std::string_view)te.term(), expected);
}

} // namespace

class TermsDictTest : public SoluxTest {
};

TEST_F(TermsDictTest, DecodesEscapedPrefixAndSuffixLengths) {
  TestIndex ti;
  TestField field(ti, "foo_s");

  std::string longPrefix(200, 'p');
  std::string sharedA = longPrefix + "a";
  std::string sharedB = longPrefix + "b";

  std::string longSuffixA = std::string(30, 'a') + std::string(180, 'x');
  std::string longSuffixB = std::string(30, 'a') + std::string(180, 'y');
  std::string suffixOnlyA = std::string("q") + std::string(200, 'a');
  std::string suffixOnlyB = std::string("r") + std::string(200, 'b');

  std::vector<std::string> terms = {
    longSuffixA, longSuffixB, sharedA, sharedB, suffixOnlyA, suffixOnlyB
  };
  std::vector<std::string> expected = terms;
  std::sort(expected.begin(), expected.end());

  indexTerms(ti, field, terms);

  EXPECT_EQ(collectNext(field), expected);
  EXPECT_EQ(collectSeekCeil(field), expected);
  expectExactSeeks(field, expected);

  expectCeil(field, longSuffixA + "!", longSuffixB);
  expectCeil(field, sharedA + "!", sharedB);
  expectCeil(field, suffixOnlyA + "!", suffixOnlyB);
}
