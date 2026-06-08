#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "solux/analysis/Analyzer.h"
#include "solux/schema/FieldType.h"
#include "test/SoluxTest.h"

using namespace solux;

namespace {

// String-in / tokens-out harness. Drains a stream over `val` and returns the
// emitted token texts. The positions vector (parallel) holds absolute positions
// derived from positionIncrement, for tests that care.
struct Analysis {
  std::vector<std::string> terms;
  std::vector<int> positions;
};

// Drive a raw stream (caller already wired the chain). `head` supplies the input.
Analysis analyze(Tokenizer& head, TokenStream& tail, std::string_view val) {
  head.setValue(val);
  Analysis out;
  Token& tok = head.getToken();
  int pos = -1;
  while (tail.incrementToken()) {
    pos += tok.positionIncrement;
    out.terms.emplace_back(tok.text);
    out.positions.push_back(pos);
  }
  return out;
}

// Drive a full chain (TokenChain from createAnalyzer), honoring reset().
Analysis analyze(TokenChain& tc, std::string_view val) {
  tc.head.setValue(val);
  tc.reset();
  return analyze(tc.head, *tc.tail, val);
}

}  // namespace

class AnalysisTest : public SoluxTest {};

TEST_F(AnalysisTest, whitespaceBasic) {
  WhitespaceTokenizer tok;
  auto out = analyze(tok, tok, "the quick brown fox");
  EXPECT_EQ((std::vector<std::string>{"the", "quick", "brown", "fox"}), out.terms);
  EXPECT_EQ((std::vector<int>{0, 1, 2, 3}), out.positions);
}

TEST_F(AnalysisTest, whitespaceCollapsesRuns) {
  WhitespaceTokenizer tok;
  // leading/trailing/internal runs of mixed whitespace produce no empty tokens
  auto out = analyze(tok, tok, "  a\t\tb \n c  ");
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), out.terms);
}

TEST_F(AnalysisTest, whitespaceEmptyAndAllSpace) {
  WhitespaceTokenizer tok;
  EXPECT_TRUE(analyze(tok, tok, "").terms.empty());
  EXPECT_TRUE(analyze(tok, tok, "   \t\n ").terms.empty());
}

TEST_F(AnalysisTest, keywordWholeInput) {
  KeywordTokenizer tok;
  auto out = analyze(tok, tok, "Hello World  ");
  EXPECT_EQ((std::vector<std::string>{"Hello World  "}), out.terms);
}

TEST_F(AnalysisTest, keywordEmptyEmitsNothing) {
  KeywordTokenizer tok;
  EXPECT_TRUE(analyze(tok, tok, "").terms.empty());
}

TEST_F(AnalysisTest, lowercaseFolds) {
  auto src = std::make_unique<WhitespaceTokenizer>();
  WhitespaceTokenizer& head = *src;
  LowercaseFilter filter(std::move(src));
  auto out = analyze(head, filter, "The QUICK bRoWn");
  EXPECT_EQ((std::vector<std::string>{"the", "quick", "brown"}), out.terms);
}

// The borrow contract: a rewriting filter must not write the source bytes.
TEST_F(AnalysisTest, lowercaseDoesNotMutateSource) {
  std::string source = "Mixed CASE Words";
  std::string original = source;

  auto src = std::make_unique<WhitespaceTokenizer>();
  WhitespaceTokenizer& head = *src;
  LowercaseFilter filter(std::move(src));
  auto out = analyze(head, filter, source);

  EXPECT_EQ((std::vector<std::string>{"mixed", "case", "words"}), out.terms);
  EXPECT_EQ(original, source);  // source value left untouched
}

// Pure-lowercase tokens pass through pointing straight at the source bytes
// (zero copy); only rewritten tokens point into the filter's buffer.
TEST_F(AnalysisTest, lowercasePassThroughBorrowsSource) {
  std::string source = "abc DEF";
  auto src = std::make_unique<WhitespaceTokenizer>();
  WhitespaceTokenizer& head = *src;
  LowercaseFilter filter(std::move(src));

  head.setValue(source);
  Token& tok = head.getToken();

  ASSERT_TRUE(filter.incrementToken());
  EXPECT_EQ("abc", tok.text);
  EXPECT_EQ(source.data(), tok.text.data());  // borrows the source directly

  ASSERT_TRUE(filter.incrementToken());
  EXPECT_EQ("def", tok.text);
  EXPECT_NE(source.data() + 4, tok.text.data());  // rewritten: lives in filter buffer

  EXPECT_FALSE(filter.incrementToken());
}

// Reusing one stream across values (the multi-valued indexing pattern) works:
// setValue resets the head cursor each time.
TEST_F(AnalysisTest, reuseAcrossValues) {
  WhitespaceTokenizer tok;
  EXPECT_EQ((std::vector<std::string>{"one", "two"}), analyze(tok, tok, "one two").terms);
  EXPECT_EQ((std::vector<std::string>{"three"}), analyze(tok, tok, "three").terms);
  EXPECT_TRUE(analyze(tok, tok, "").terms.empty());
  EXPECT_EQ((std::vector<std::string>{"four", "five"}), analyze(tok, tok, "four five").terms);
}

// End-to-end through the schema-configured chain factory.
TEST_F(AnalysisTest, chainWhitespaceLowercase) {
  TextFieldType ft("body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace", {"lowercase"});
  auto chain = ft.createAnalyzer("body");
  ASSERT_NE(nullptr, chain);
  EXPECT_FALSE(chain->stateful);
  auto out = analyze(*chain, "Hello World FOO");
  EXPECT_EQ((std::vector<std::string>{"hello", "world", "foo"}), out.terms);
}

TEST_F(AnalysisTest, chainKeyword) {
  TextFieldType ft("k", FieldType::INDEX_DOCS, "keyword");
  auto chain = ft.createAnalyzer("k");
  auto out = analyze(*chain, "Keep As Is");
  EXPECT_EQ((std::vector<std::string>{"Keep As Is"}), out.terms);
}

// The collapsed tokenizer: "nocopy_whitespace" is still accepted as an alias and
// resolves to the same WhitespaceTokenizer behavior.
TEST_F(AnalysisTest, nocopyWhitespaceAlias) {
  TextFieldType ft("w", FieldType::INDEX_DOCS_FREQS_POSITIONS, "nocopy_whitespace");
  auto chain = ft.createAnalyzer("w");
  auto out = analyze(*chain, "a b c");
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), out.terms);
}
