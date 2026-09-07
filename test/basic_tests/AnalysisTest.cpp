#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uni_algo/case.h>
#include <uni_algo/norm.h>
#include <uni_algo/ranges_word.h>

#include "luxir/analysis/Analyzer.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/FieldType.h"
#include "test/LuxirTest.h"

using namespace luxir;

namespace {

// String-in / tokens-out harness. Drains a stream over `val` and returns the
// emitted token texts. The positions vector (parallel) holds absolute positions
// derived from positionIncrement, for tests that care.
struct Analysis {
  std::vector<std::string> terms;
  std::vector<int> positions;
  std::vector<int> starts;  // startOffset per token (parallel to terms)
  std::vector<int> ends;    // endOffset per token
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
    out.starts.push_back(tok.startOffset);
    out.ends.push_back(tok.endOffset);
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

class AnalysisTest : public LuxirTest {};

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

// --- Token offsets ----------------------------------------------------------

TEST_F(AnalysisTest, whitespaceOffsets) {
  WhitespaceTokenizer tok;
  //                              0123456789012345678
  auto out = analyze(tok, tok, "the quick brown fox");
  EXPECT_EQ((std::vector<int>{0, 4, 10, 16}), out.starts);
  EXPECT_EQ((std::vector<int>{3, 9, 15, 19}), out.ends);
}

// Leading/internal/trailing whitespace runs do not perturb offsets: each token's
// span is the byte range it actually occupies in the source.
TEST_F(AnalysisTest, whitespaceOffsetsAcrossRuns) {
  WhitespaceTokenizer tok;
  //                              0 1 23 4 5 6 7 89 0 1
  auto out = analyze(tok, tok, "  a\t\tb \n c  ");
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), out.terms);
  EXPECT_EQ((std::vector<int>{2, 5, 9}), out.starts);
  EXPECT_EQ((std::vector<int>{3, 6, 10}), out.ends);
}

TEST_F(AnalysisTest, keywordOffsetsSpanWholeInput) {
  KeywordTokenizer tok;
  auto out = analyze(tok, tok, "Hello World  ");
  EXPECT_EQ((std::vector<int>{0}), out.starts);
  EXPECT_EQ((std::vector<int>{13}), out.ends);  // includes trailing spaces
}

// The key contract: a rewriting filter repoints `text` at its own buffer but
// must leave the offset pointing back at the original source span.
TEST_F(AnalysisTest, offsetSurvivesRewritingFilter) {
  auto src = std::make_unique<WhitespaceTokenizer>();
  WhitespaceTokenizer& head = *src;
  LowercaseFilter filter(std::move(src));
  //                                  0123456789
  auto out = analyze(head, filter, "The QUICK");
  EXPECT_EQ((std::vector<std::string>{"the", "quick"}), out.terms);  // text rewritten
  EXPECT_EQ((std::vector<int>{0, 4}), out.starts);                   // offsets unchanged
  EXPECT_EQ((std::vector<int>{3, 9}), out.ends);
}

TEST_F(AnalysisTest, lowercaseFolds) {
  auto src = std::make_unique<WhitespaceTokenizer>();
  WhitespaceTokenizer& head = *src;
  LowercaseFilter filter(std::move(src));
  auto out = analyze(head, filter, "The QUICK bRoWn");
  EXPECT_EQ((std::vector<std::string>{"the", "quick", "brown"}), out.terms);
}

// Unicode lowercase, and only lowercase: accents and sharp-s survive (no
// casefold, no accent fold), compatibility forms are not normalized (fullwidth
// stays fullwidth) - the whitespace family touches nothing but case.
TEST_F(AnalysisTest, lowercaseUnicode) {
  auto src = std::make_unique<WhitespaceTokenizer>();
  WhitespaceTokenizer& head = *src;
  LowercaseFilter filter(std::move(src));
  auto out = analyze(head, filter, "CAFÉ Straße ＡＢＣ");
  EXPECT_EQ((std::vector<std::string>{"café", "straße", "ａｂｃ"}), out.terms);
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

// End-to-end through the schema-configured chain factory: the _wl chain,
// whitespace + Unicode lowercase. Stateless (no segmentation cursor),
// punctuation kept (whitespace family), accents and compatibility forms
// preserved - it touches nothing but case.
TEST_F(AnalysisTest, chainWhitespaceLowercase) {
  TextFieldType ft("body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace", {"lowercase"});
  auto chain = ft.createAnalyzer("body");
  ASSERT_NE(nullptr, chain);
  EXPECT_FALSE(chain->stateful);
  auto out = analyze(*chain, "Hello World FOO");
  EXPECT_EQ((std::vector<std::string>{"hello", "world", "foo"}), out.terms);
  out = analyze(*chain, "The QUICK Straße CAFÉ!");
  EXPECT_EQ((std::vector<std::string>{"the", "quick", "straße", "café!"}), out.terms);
}

TEST_F(AnalysisTest, chainKeyword) {
  TextFieldType ft("k", FieldType::INDEX_DOCS, "keyword");
  auto chain = ft.createAnalyzer("k");
  auto out = analyze(*chain, "Keep As Is");
  EXPECT_EQ((std::vector<std::string>{"Keep As Is"}), out.terms);
}

// The registry is the only path from a name to a stage: an unknown name (the
// old "nocopy_whitespace" alias is gone) is a teaching error naming the valid set.
TEST_F(AnalysisTest, unknownComponentIsRejected) {
  try {
    TextFieldType ft("w", FieldType::INDEX_DOCS_FREQS_POSITIONS, "nocopy_whitespace");
    FAIL() << "unknown tokenizer accepted";
  } catch (const std::invalid_argument& e) {
    EXPECT_EQ("unknown tokenizer 'nocopy_whitespace'; valid tokenizers: whitespace, keyword, unicode_word",
              std::string(e.what()));
  }
  try {
    TextFieldType ft("w", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace", {"stemmer"});
    FAIL() << "unknown filter accepted";
  } catch (const std::invalid_argument& e) {
    EXPECT_EQ("unknown filter 'stemmer'; valid filters: lowercase, nfkc_cf, fold", std::string(e.what()));
  }
}

// The compiled Analyzer owns its configuration: chains stay valid after the
// authored definition and the arena backing its views are gone.
TEST_F(AnalysisTest, analyzerOutlivesDefinition) {
  std::shared_ptr<const Analyzer> analyzer;
  {
    std::pmr::monotonic_buffer_resource arena;
    api::AnalyzerDef def;
    std::string err;
    ASSERT_TRUE(api::read_json(def, R"({"tokenizer":"unicode_word","filters":["nfkc_cf","fold"]})", arena, &err))
        << err;
    analyzer = Analyzer::compile(def);
  }
  EXPECT_TRUE(analyzer->fusedHead);
  EXPECT_EQ("unicode_word", analyzer->tokenizer->name);
  ASSERT_EQ(2u, analyzer->filters.size());
  EXPECT_EQ("fold", analyzer->filters[1]->name);
  auto chain = analyzer->createChain();
  EXPECT_EQ((std::vector<std::string>{"cafe", "strasse"}), analyze(*chain, "CAFÉ Straße").terms);
}

// ---------------------------------------------------------------------------
// Unicode conformance spike (uni-algo, vendored, Unicode 15.1.0).
//
// Gates the analysis stack before the real UnicodeWordTokenizer / NFKC_CF filter
// are built on top of uni-algo: validates that the pinned library actually
// passes UAX#29 word segmentation and Unicode full case folding, plus a few
// shipped-behavior sanity checks. The two file-driven tests consume the official
// Unicode data files that cmake downloads into $TMP/luxir; they GTEST_SKIP when
// the files are absent (offline) rather than fail.
// ---------------------------------------------------------------------------
namespace {
namespace fs = std::filesystem;

// Path to a Unicode conformance file (cmake downloads these next to book.txt).
fs::path unicodeDataPath(const char* name) {
  return fs::temp_directory_path() / "luxir" / name;
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
  if (!memtrack::counting_enabled) GTEST_SKIP() << "allocation counter disabled under ASan";
  std::string longUni;
  for (int i = 0; i < 500; i++) longUni += "Ä";  // long, non-SSO, non-ASCII
  memtrack::AllocScope s;
  std::string folded = una::cases::to_casefold_utf8(longUni);
  long allocs = s.count();  // capture before any EXPECT (gtest macros allocate)
  EXPECT_GT(allocs, 0);
  EXPECT_FALSE(folded.empty());
}

// The real analysis chain: unicode_word tokenizer + nfkc_cf filter, via the
// schema factory. UAX#29 word segmentation, NFKC_CF folding, punctuation dropped.
TEST_F(AnalysisTest, chainUnicodeWordNfkcCf) {
  TextFieldType ft("body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word", {"nfkc_cf"});
  auto chain = ft.createAnalyzer("body");
  ASSERT_NE(nullptr, chain);
  EXPECT_TRUE(chain->stateful);  // unicode_word carries a segmentation cursor
  auto out = analyze(*chain, "The QUICK 中文 Straße café!");
  EXPECT_EQ((std::vector<std::string>{"the", "quick", "中", "文", "strasse", "café"}), out.terms);
  EXPECT_EQ((std::vector<int>{0, 1, 2, 3, 4, 5}), out.positions);
}

// The default _t chain: unicode_word + nfkc_cf + fold. Accents are removed so an
// accented word matches its bare form; CJK and case still handled.
TEST_F(AnalysisTest, chainUnicodeWordFold) {
  TextFieldType ft("t", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word", {"nfkc_cf", "fold"});
  auto chain = ft.createAnalyzer("t");
  auto out = analyze(*chain, "Café NAÏVE señor Straße 中文");
  EXPECT_EQ((std::vector<std::string>{"cafe", "naive", "senor", "strasse", "中", "文"}), out.terms);
}

// fold (_t) vs no-fold (_un): the accent is dropped with fold, preserved without.
TEST_F(AnalysisTest, foldVsPreserveAccents) {
  TextFieldType folding("t", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word", {"nfkc_cf", "fold"});
  TextFieldType preserving("un", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word", {"nfkc_cf"});
  EXPECT_EQ((std::vector<std::string>{"cafe"}), analyze(*folding.createAnalyzer("t"), "Café").terms);
  EXPECT_EQ((std::vector<std::string>{"café"}), analyze(*preserving.createAnalyzer("un"), "Café").terms);
}

// NFKC_CF must reach the fold/normalize fixpoint: NFKC of U+03D3 (ϓ) is U+038E,
// an UPPERCASE Ύ, so a single casefold-then-NFKC pass would leave uppercase in
// the "casefolded" index and canonically-equivalent query forms would miss.
TEST_F(AnalysisTest, nfkcCfReachesFixpoint) {
  TextFieldType ft("t", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word", {"nfkc_cf"});
  auto out = analyze(*ft.createAnalyzer("t"), "ϓ Ύ ύ");  // U+03D3, U+038E, U+03CD
  EXPECT_EQ((std::vector<std::string>{"ύ", "ύ", "ύ"}), out.terms);  // all fold to U+03CD
}

// The fused StandardTokenizer (swapped in for unicode_word + nfkc_cf) must produce
// byte-identical tokens to the explicit two-stage chain - the optimization is
// behavior-preserving.
TEST_F(AnalysisTest, standardFusionMatchesComposedChain) {
  for (std::string_view v : {"The QUICK brown FOX 42",
                             "Café 中文 Straße señor naïve",
                             "  ...!?  mixed-CASE  ",
                             "ﬃ ligature ＡＢＣ fullwidth"}) {
    auto t1 = makeUnicodeWordTokenizer();
    Tokenizer& h1 = *t1;
    auto composedTail = makeNfkcCasefoldFilter(std::move(t1));
    TokenChain composed(h1, std::move(composedTail), true);

    auto t2 = makeStandardTokenizer();
    Tokenizer& h2 = *t2;
    std::unique_ptr<TokenStream> fusedTail = std::move(t2);
    TokenChain fused(h2, std::move(fusedTail), true);

    EXPECT_EQ(analyze(composed, v).terms, analyze(fused, v).terms) << "mismatch on: " << v;
  }
}

namespace {
// Build the composed reference chain (unicode_word + nfkc_cf, pure uni-algo)
// and the fused StandardTokenizer (region dispatch + ASCII DFA + bulk
// lowercase), and return both term lists for `v`. The fast paths are correct
// iff these are always identical.
std::pair<std::vector<std::string>, std::vector<std::string>> composedVsFused(std::string_view v) {
  auto t1 = makeUnicodeWordTokenizer();
  Tokenizer& h1 = *t1;
  std::unique_ptr<TokenStream> composedTail = makeNfkcCasefoldFilter(std::move(t1));
  TokenChain composed(h1, std::move(composedTail), true);

  auto t2 = makeStandardTokenizer();
  Tokenizer& h2 = *t2;
  std::unique_ptr<TokenStream> fusedTail = std::move(t2);
  TokenChain fused(h2, std::move(fusedTail), true);

  return {analyze(composed, v).terms, analyze(fused, v).terms};
}
}  // namespace

// Adversarial equivalence for the StandardTokenizer fast paths: the safe-split
// region dispatch, the ASCII word-break DFA, and the bulk ASCII lowercase must
// be invisible next to the composed chain. Curly apostrophes/quotes and accents
// exercise mixed ASCII/non-ASCII region routing; mids and underscores the DFA
// join rules; format/combining/emoji codepoints the region boundary logic.
TEST_F(AnalysisTest, standardFusionAdversarialEquivalence) {
  const char* cases[] = {
      "don’t can’t won’t",                  // curly apostrophe is MidNumLet
      "“Quoted” and—dashed",                // curly quotes, em dash
      "Pávlovna said don't go before 3.14 or 1,000",
      "a:b 1:2 a,b b;c 1;2 a.b.c x..y can't. 'tis 'quoted'",
      "_ __ _x x_ a_1 x_:y 1_.2 _'_",
      "tab\tsep\r\nlines\vvtab\fformfeed  double  space",
      "x\u00ady soft\u00adhyphen",             // U+00AD soft hyphen: Format, glued by WB4
      "combining a\u0301 mark \u0301orphan-after-space",  // U+0301 combining acute
      "nb\u00a0sp word\u00a0joined",            // NBSP suppresses the safe split
      "ＡＢＣ １２ fullwidth",         // NFKC maps to ASCII
      "ﬃ ligature ﬁnal",
      "中文mixed汉字words",                // CJK adjacent to ASCII
      "Большой ТЕАТР",
      "ΣΟΦΟΣ ὈΔΥΣΣΕΎΣ",
      "\U0001f1fa\U0001f1f8\U0001f1eb\U0001f1f7 flags \U0001f44d\U0001f3fb emoji",
      " \t leading and trailing \r\n ",
      "...  !!!  ---  ()[]{}",
      "ALLCAPS MixedCase lowercase 0123456789",
      "",
  };
  for (std::string_view v : cases) {
    auto [composed, fused] = composedVsFused(v);
    EXPECT_EQ(composed, fused) << "mismatch on: " << v;
  }
}

// Every WordBreakTest input sequence through both chains. The corpus is dense
// in exotic boundary machinery (ZWJ, regional indicators, Extend/Format runs,
// CR LF, WSegSpace) and stress-tests the safe-split region scanner against
// codepoints it must route to the conformant path.
TEST_F(AnalysisTest, standardFusionMatchesComposedOnWordBreakCorpus) {
  fs::path path = unicodeDataPath("WordBreakTest-15.1.0.txt");
  std::ifstream in(path, std::ios::binary);
  if (!in) GTEST_SKIP() << "missing " << path << " (reconfigure cmake to download)";

  int total = 0;
  for (std::string l; std::getline(in, l);) {
    if (auto h = l.find('#'); h != std::string::npos) l.resize(h);
    std::string input;
    std::istringstream ts(l);
    for (std::string tok; ts >> tok;) {
      if (tok != "\xC3\xB7" && tok != "\xC3\x97")  // skip break/no-break marks
        utf8Append(input, (uint32_t) std::stoul(tok, nullptr, 16));
    }
    if (input.empty()) continue;
    ++total;
    auto [composed, fused] = composedVsFused(input);
    ASSERT_EQ(composed, fused) << "WordBreakTest input: " << l;
  }
  RecordProperty("wordbreak_equivalence_lines", total);
}

// Randomized equivalence: values assembled from atoms chosen to stress the
// region splitter (every ASCII whitespace byte, mids, curly punctuation,
// accents, CJK, combining marks, format chars, emoji/RI, fullwidth forms).
TEST_F(AnalysisTest, standardFusionRandomizedEquivalence) {
  const char* atoms[] = {
      "the", "Quick", "BROWN", "fox42", "3.14", "1,000", "a:b", "1:2", "x_y", "_",
      "can't", "x..y", ".", ",", ":", ";", "'", "\"", "-", "(", ")",
      " ", "  ", "\t", "\n", "\r\n", "\v", "\f",
      "don’t", "“", "”", "—", "café", "Pávlovna",
      "straße", "中文", "\u0301", "\u00ad", "\u200d", "\u00a0",
      "ＡＢ", "\U0001f1fa\U0001f1f8", "\U0001f44d",
  };
  int natoms = (int) (sizeof(atoms) / sizeof(atoms[0]));
  for (int round = 0; round < 300; round++) {
    std::string v;
    int n = (int) rng.rint(40);
    for (int i = 0; i < n; i++) v += atoms[rng.rint(natoms)];
    auto [composed, fused] = composedVsFused(v);
    ASSERT_EQ(composed, fused) << "value: " << v;
  }
}

// Pin the ASCII fast-path word-break semantics directly (all pure-ASCII, so
// these stay on the byte-class DFA): '.'/'\'' join same-kind pairs, ':' joins
// letters only, ',' joins digits only, '_' joins but is not a word by itself,
// and a trailing mid char never joins.
TEST_F(AnalysisTest, standardAsciiWordBreakSemantics) {
  TextFieldType ft("wl", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word", {"nfkc_cf"});
  auto chain = ft.createAnalyzer("wl");
  EXPECT_EQ((std::vector<std::string>{"can't", "stop", "won't", "3.14", "1,000", "a:b", "1", "2",
                                      "a", "b", "_tag", "x_1", "a.b.c", "x", "y"}),
            analyze(*chain, "Can't STOP won't 3.14 1,000 a:b 1:2 a,b _tag x_1 a.b.c x..y").terms);
  EXPECT_EQ((std::vector<std::string>{"x"}), analyze(*chain, "_ x").terms);
  EXPECT_EQ((std::vector<std::string>{"end"}), analyze(*chain, "end.").terms);
}

// Offsets through the fused StandardTokenizer, both the ASCII DFA fast path and
// the uni-algo path. They are byte spans into the original value, surviving the
// lowercasing/NFKC_CF rewrite of `text`.
TEST_F(AnalysisTest, standardOffsets) {
  TextFieldType ft("wl", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word", {"nfkc_cf"});
  auto chain = ft.createAnalyzer("wl");

  //                                  0123456789012345678
  auto ascii = analyze(*chain, "The Quick BROWN fox");
  EXPECT_EQ((std::vector<std::string>{"the", "quick", "brown", "fox"}), ascii.terms);
  EXPECT_EQ((std::vector<int>{0, 4, 10, 16}), ascii.starts);
  EXPECT_EQ((std::vector<int>{3, 9, 15, 19}), ascii.ends);

  // Non-ASCII region routed to uni-algo. "café" is 5 bytes (é is 2); the offset
  // is the source span even though NFKC_CF repoints text at its scratch buffer.
  auto wide = analyze(*chain, "café World");
  EXPECT_EQ((std::vector<std::string>{"café", "world"}), wide.terms);
  EXPECT_EQ((std::vector<int>{0, 6}), wide.starts);
  EXPECT_EQ((std::vector<int>{5, 11}), wide.ends);
}

// Offsets through the standalone unicode_word tokenizer (no nfkc fusion). Each
// CJK ideograph is its own 3-byte segment.
TEST_F(AnalysisTest, unicodeWordOffsets) {
  TextFieldType ft("w", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word");
  auto chain = ft.createAnalyzer("w");
  auto out = analyze(*chain, "Hello 中文 World");
  EXPECT_EQ((std::vector<std::string>{"Hello", "中", "文", "World"}), out.terms);
  EXPECT_EQ((std::vector<int>{0, 6, 9, 13}), out.starts);
  EXPECT_EQ((std::vector<int>{5, 9, 12, 18}), out.ends);
}

// The ASCII path must be allocation-free per value once buffers are warm: the
// bulk lowercase reuses `lowered`, DFA tokens are views into it, and no
// uni-algo view or NFKC scratch is touched for pure-ASCII regions.
TEST_F(AnalysisTest, standardAsciiPathAllocationFree) {
  if (!memtrack::counting_enabled) GTEST_SKIP() << "allocation counter disabled under ASan";
  auto t = makeStandardTokenizer();
  Tokenizer& head = *t;
  std::unique_ptr<TokenStream> tail = std::move(t);
  std::string_view v = "The Quick brown FOX jumps over 42 lazy dogs don't stop a:b 3.14";
  size_t bytes = 0;
  auto run = [&] {
    head.setValue(v);
    tail->reset();
    while (tail->incrementToken()) bytes += head.getToken().text.size();
  };
  run();  // warm the lowered-value buffer
  memtrack::AllocScope s;
  run();
  long allocs = s.count();  // capture before any EXPECT (gtest macros allocate)
  EXPECT_EQ(0, allocs);
  EXPECT_GT(bytes, 0u);
}

// unicode_word without a fold filter: segmentation only, original bytes preserved.
TEST_F(AnalysisTest, chainUnicodeWordNoFilter) {
  TextFieldType ft("w", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word");
  auto chain = ft.createAnalyzer("w");
  auto out = analyze(*chain, "Hello 中文 World");
  EXPECT_EQ((std::vector<std::string>{"Hello", "中", "文", "World"}), out.terms);
}

// Reusing one stateful chain across values: reset() rebuilds the segmentation
// cursor each time, including empty/punctuation-only values.
TEST_F(AnalysisTest, unicodeWordReuseAcrossValues) {
  TextFieldType ft("b", FieldType::INDEX_DOCS_FREQS_POSITIONS, "unicode_word", {"nfkc_cf"});
  auto chain = ft.createAnalyzer("b");
  EXPECT_EQ((std::vector<std::string>{"one", "two"}), analyze(*chain, "One TWO").terms);
  EXPECT_EQ((std::vector<std::string>{"中", "文"}), analyze(*chain, "中文").terms);
  EXPECT_TRUE(analyze(*chain, "  ...  ").terms.empty());
  EXPECT_EQ((std::vector<std::string>{"café"}), analyze(*chain, "CAFÉ").terms);
}

// UAX#29 word segmentation yields string_views into the source, so iterating it
// must not allocate - the hot-path property the real tokenizer relies on. Checked
// for both ASCII and multibyte input.
TEST_F(AnalysisTest, wordSegmentationIsAllocationFree) {
  if (!memtrack::counting_enabled) GTEST_SKIP() << "allocation counter disabled under ASan";
  // Returns {heap allocations, total segment bytes}; the byte count keeps the
  // loop observable and lets us assert it actually produced segments.
  auto segAllocs = [](std::string_view in) {
    size_t bytes = 0;
    memtrack::AllocScope s;
    for (std::string_view w : una::views::word_only::utf8(in)) bytes += w.size();
    long allocs = s.count();
    return std::pair<long, size_t>(allocs, bytes);
  };
  std::string ascii = "the quick brown fox jumps over the lazy dog";
  std::string uni = "café 中文 naïve Ärger test";

  segAllocs(ascii);  // warm up (settle any one-time init)
  segAllocs(uni);
  auto [asciiAllocs, asciiBytes] = segAllocs(ascii);
  auto [uniAllocs, uniBytes] = segAllocs(uni);
  EXPECT_EQ(0, asciiAllocs);
  EXPECT_EQ(0, uniAllocs);
  EXPECT_GT(asciiBytes, 0u);  // the loops really produced segments
  EXPECT_GT(uniBytes, 0u);
}
