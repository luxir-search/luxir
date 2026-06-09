#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <uni_algo/case.h>
#include <uni_algo/norm.h>
#include <uni_algo/ranges_word.h>

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

// ---------------------------------------------------------------------------
// Unicode conformance spike (uni-algo, vendored, Unicode 15.1.0).
//
// Gates the analysis stack before the real UnicodeWordTokenizer / NFKC_CF filter
// are built on top of uni-algo: validates that the pinned library actually
// passes UAX#29 word segmentation and Unicode full case folding, plus a few
// shipped-behavior sanity checks. The two file-driven tests consume the official
// Unicode data files that cmake downloads into $TMP/solux; they GTEST_SKIP when
// the files are absent (offline) rather than fail.
// ---------------------------------------------------------------------------
namespace {
namespace fs = std::filesystem;

// Path to a Unicode conformance file (cmake downloads these next to book.txt).
fs::path unicodeDataPath(const char* name) {
  return fs::temp_directory_path() / "solux" / name;
}

// toNFKC_Casefold approximation for the spike: case-fold, then NFKC. The
// single-pass NFKC_CF mapping is a later optimization; this two-pass form
// matches it for the cases exercised here.
std::string nfkc_cf(std::string_view s) {
  return una::norm::to_nfkc_utf8(una::cases::to_casefold_utf8(s));
}

// Append code point cp to out as UTF-8.
void utf8Append(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back((char) cp);
  } else if (cp <= 0x7FF) {
    out.push_back((char) (0xC0 | (cp >> 6)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back((char) (0xE0 | (cp >> 12)));
    out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  } else {
    out.push_back((char) (0xF0 | (cp >> 18)));
    out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  }
}

// UAX#29 word-only tokenization (letter/number-bearing segments; punctuation and
// whitespace dropped) - the shape the real tokenizer will take.
std::vector<std::string> wordTokens(std::string_view s) {
  std::vector<std::string> out;
  for (std::string_view w : una::views::word_only::utf8(s)) out.emplace_back(w);
  return out;
}
}  // namespace

// Full Unicode case folding: ASCII lowercases, sharp-s and ligatures expand.
TEST_F(AnalysisTest, uniAlgoCasefoldSanity) {
  EXPECT_EQ("strasse", una::cases::to_casefold_utf8("STRASSE"));
  EXPECT_EQ("strasse", una::cases::to_casefold_utf8("Straße"));  // sharp s -> ss
  EXPECT_EQ("ffi", una::cases::to_casefold_utf8("ﬃ"));           // U+FB03 ligature ffi
}

// NFKC_CF collapses full-width and unifies composed vs decomposed forms.
TEST_F(AnalysisTest, uniAlgoNfkcCfSanity) {
  EXPECT_EQ("abc", nfkc_cf("ＡＢＣ"));  // full-width U+FF21..FF23 -> "abc"
  // "cafe" composed (U+00E9) vs decomposed (e + U+0301) fold identically.
  // \u escapes here on purpose: the byte-level encoding difference is the test.
  EXPECT_EQ(nfkc_cf("caf\u00e9"), nfkc_cf("cafe\u0301"));
}

// UAX#29 baseline: one token per CJK ideograph; latin words stay whole;
// punctuation is dropped. ("hello 中文 world!")
TEST_F(AnalysisTest, uniAlgoWordSegmentationBaseline) {
  auto t = wordTokens("hello \xE4\xB8\xAD\xE6\x96\x87 world!");
  EXPECT_EQ((std::vector<std::string>{"hello", "\xE4\xB8\xAD", "\xE6\x96\x87", "world"}), t);
}

// The ASCII fast path the design relies on: NFKC_CF on ASCII is plain lowercase,
// so a scalar lowercase produces exactly the uni-algo result for ASCII input.
TEST_F(AnalysisTest, nfkcCfOnAsciiIsLowercase) {
  EXPECT_EQ("the quick brown fox 42", nfkc_cf("The QUICK Brown fox 42"));
}

// Full UAX#29 word-boundary conformance against the official 15.1.0 test file.
// Each line encodes break (U+00F7) / no-break (U+00D7) positions between code
// points; we compare uni-algo's segment boundaries to the expected set.
TEST_F(AnalysisTest, uax29WordBreakConformance) {
  fs::path path = unicodeDataPath("WordBreakTest-15.1.0.txt");
  std::ifstream in(path, std::ios::binary);
  if (!in) GTEST_SKIP() << "missing " << path << " (reconfigure cmake to download)";

  int total = 0, failures = 0, firstFailLine = 0;
  std::string line;
  for (int lineNo = 1; std::getline(in, line); ++lineNo) {
    if (auto h = line.find('#'); h != std::string::npos) line.resize(h);

    std::string input;
    std::set<size_t> expected;  // break byte-offsets
    bool started = false;
    std::istringstream ts(line);
    for (std::string tok; ts >> tok;) {
      if (tok == "\xC3\xB7") {  // U+00F7 division sign: boundary here
        expected.insert(input.size());
        started = true;
      } else if (tok == "\xC3\x97") {  // U+00D7 multiplication sign: no boundary
        started = true;
      } else {
        utf8Append(input, (uint32_t) std::stoul(tok, nullptr, 16));
      }
    }
    if (!started || input.empty()) continue;

    ++total;
    std::set<size_t> actual;
    for (std::string_view seg : una::views::word::utf8(std::string_view(input)))
      actual.insert((size_t) (seg.data() - input.data()));
    actual.insert(input.size());

    if (actual != expected && failures++ == 0) firstFailLine = lineNo;
  }
  EXPECT_EQ(0, failures) << failures << " of " << total
                         << " UAX#29 word-break lines diverge; first at line " << firstFailLine;
  RecordProperty("wordbreak_lines", total);
}

// Full Unicode case-folding conformance: every common (C) and full (F) mapping
// in CaseFolding.txt must round-trip through to_casefold_utf8. (S simple / T
// Turkic rows are intentionally skipped - we fold locale-independently.)
TEST_F(AnalysisTest, caseFoldingConformance) {
  fs::path path = unicodeDataPath("CaseFolding-15.1.0.txt");
  std::ifstream in(path, std::ios::binary);
  if (!in) GTEST_SKIP() << "missing " << path << " (reconfigure cmake to download)";

  auto trim = [](std::string s) {
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
  };

  int total = 0, failures = 0;
  uint32_t firstFailCode = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
    std::vector<std::string> f;
    std::istringstream ls(line);
    for (std::string field; std::getline(ls, field, ';');) f.push_back(trim(field));
    if (f.size() < 3) continue;
    if (f[1] != "C" && f[1] != "F") continue;  // full folding = C + F rows

    std::string src;
    utf8Append(src, (uint32_t) std::stoul(f[0], nullptr, 16));
    std::string expected;
    std::istringstream ms(f[2]);
    for (std::string cp; ms >> cp;) utf8Append(expected, (uint32_t) std::stoul(cp, nullptr, 16));

    ++total;
    if (una::cases::to_casefold_utf8(src) != expected && failures++ == 0)
      firstFailCode = (uint32_t) std::stoul(f[0], nullptr, 16);
  }
  EXPECT_EQ(0, failures) << failures << " of " << total
                         << " case-folding rows diverge; first at U+" << std::hex << firstFailCode;
  RecordProperty("casefold_rows", total);
}

// Guard: the allocation counter actually observes allocations, so the zero-alloc
// assertion below cannot pass by a no-op/broken counter. The fold path returns a
// std::string and must allocate for a long non-ASCII token.
TEST_F(AnalysisTest, allocCounterObservesFoldAllocation) {
  std::string longUni;
  for (int i = 0; i < 500; i++) longUni += "Ä";  // long, non-SSO, non-ASCII
  memtrack::AllocScope s;
  std::string folded = una::cases::to_casefold_utf8(longUni);
  long allocs = s.count();  // capture before any EXPECT (gtest macros allocate)
  EXPECT_GT(allocs, 0);
  EXPECT_FALSE(folded.empty());
}

// UAX#29 word segmentation yields string_views into the source, so iterating it
// must not allocate - the hot-path property the real tokenizer relies on. Checked
// for both ASCII and multibyte input.
TEST_F(AnalysisTest, wordSegmentationIsAllocationFree) {
  static volatile size_t sink = 0;
  auto segAllocs = [](std::string_view in) {
    size_t local = 0;
    memtrack::AllocScope s;
    for (std::string_view w : una::views::word_only::utf8(in)) local += w.size();
    long allocs = s.count();
    sink = local;  // observable store: forces the loop to actually run
    return allocs;
  };
  std::string ascii = "the quick brown fox jumps over the lazy dog";
  std::string uni = "café 中文 naïve Ärger test";

  segAllocs(ascii);  // warm up (settle any one-time init)
  segAllocs(uni);
  EXPECT_EQ(0, segAllocs(ascii));
  EXPECT_EQ(0, segAllocs(uni));
}
