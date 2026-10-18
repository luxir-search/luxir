// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/analysis/Analyzer.h"
#include "luxir/analysis/AsciiScan.h"
#include "luxir/analysis/KStemmer.h"
#include "luxir/analysis/NfkcCfData.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "luxir/api/luxir_types.hpp"
#include <glaze/glaze.hpp>

#include <uni_algo/case.h>
#include <uni_algo/norm.h>
#include <uni_algo/ranges_word.h>
#include <uni_algo/ranges_conv.h>
#include <uni_algo/version.h>

// The uni-algo dependency is confined to this translation unit; the rest of the
// codebase sees only the factory functions declared in Analyzer.h. uni-algo is
// pinned/vendored (Unicode 15.1.0) - its version is part of the index format.

namespace luxir {

namespace {

static_assert(UNI_ALGO_UNICODE_VERSION == nfkc_cf_data::unicodeVersion,
              "Regenerate NFKC_CF tables when upgrading uni-algo's Unicode data");

// UAX#29 word-boundary tokenizer. Emits word-like segments only (letters,
// numbers, ideographs, ...); punctuation and whitespace runs are dropped, like
// Lucene's StandardTokenizer. Each token is a string_view into the source (no
// copy), honoring the borrow contract. Stateful: it holds a segmentation cursor
// over the current value and rebuilds it in reset().
class UnicodeWordTokenizer : public Tokenizer {
  using View = decltype(una::views::word_only::utf8(std::declval<std::string_view>()));
  using Iter = decltype(std::declval<View&>().begin());
  std::optional<View> view;  // word_only view over the current value
  Iter it{};                 // cursor into `view`

public:
  // setValue (run by the chain before reset) has populated start_/end_.
  void reset() override {
    view.emplace(una::views::word_only::utf8(std::string_view(start_, (size_t) (end_ - start_))));
    it = view->begin();
  }

  bool incrementToken() override {
    token.clear();
    if (!view || it == view->end()) return false;
    std::string_view w = *it;  // word_only yields a view into the source value
    token.text = w;
    token.setOffset((int) (w.data() - base_), (int) (w.data() + w.size() - base_));
    ++it;
    return true;
  }
};

// Conservative NFC + stream-safe quick check. Counts use NFKD, as required
// by the Stream-Safe Text Process (and by uni-algo), rather than just ccc != 0.
struct NfcQuickCheck {
  unsigned lastCcc = 0;
  unsigned nonstarters = 0;
  bool yes = true;

  void add(const nfkc_cf_data::Record& record) {
    unsigned props = record.props;
    unsigned ccc = props & 255;
    unsigned lead = (props >> 9) & 3;
    nonstarters = lead ? nonstarters + lead : (props >> 11);
    if ((props & 256) || (ccc && ccc < lastCcc) || nonstarters > 30) yes = false;
    lastCcc = ccc;
  }
};

bool isNfcQuickYes(std::string_view in) {
  NfcQuickCheck check;
  for (char32_t cp : una::views::utf8(in)) {
    check.add(nfkc_cf_data::lookup((uint32_t) cp));
    if (!check.yes) return false;
  }
  return true;
}

// Unicode 15.1.0 toNFKC_Casefold: map each scalar through NFKC_CF (including
// default-ignorable removal), then NFC. No iteration. uni-algo's NFC is
// stream-safe: it inserts CGJ U+034F when NFKD would exceed 30 non-starters.
// Such adversarial runs are deterministic and idempotent, but can differ from
// ICU's bytes. Decoding uses uni-algo's bounded UTF-8 view, with U+FFFD per
// maximal subpart on ill-formed input, just like its normalization routines.
// Returns `in` itself when unchanged and NFC quick-check Yes in canonical order
// and stream-safe. Keep the common ASCII tiers independent of Unicode tables.
std::string_view applyNfkcCf(std::string_view in, std::string& buf) {
  bool hasUpper = false, hasNonAscii = false;
  for (unsigned char c : in) {
    if (c & 0x80) { hasNonAscii = true; break; }
    if (c >= 'A' && c <= 'Z') hasUpper = true;
  }
  if (hasNonAscii) {
    NfcQuickCheck check;
    bool rewritten = false;
    auto decoded = una::views::utf8(in);
    for (auto it = decoded.begin(); it != decoded.end(); ++it) {
      uint32_t cp = (uint32_t) *it;
      const auto& record = nfkc_cf_data::lookup(cp);
      if (!rewritten && check.yes) check.add(record);
      std::string_view bytes(it.begin(), (size_t) (it.end() - it.begin()));
      bool invalid = cp == 0xFFFD && bytes != "\xef\xbf\xbd";
      if (record.mapping || invalid) {
        if (!rewritten) {
          buf.assign(in.data(), (size_t) (it.begin() - in.begin()));
          rewritten = true;
        }
        if (invalid) buf.append("\xef\xbf\xbd");
        else buf.append((const char*) nfkc_cf_data::utf8 + (record.mapping & 0xFFFFFF),
                        (record.mapping >> 24) & 127);
      } else if (rewritten) {
        buf.append(bytes);
      }
    }
    if (rewritten) {
      if (!isNfcQuickYes(buf)) buf = una::norm::to_nfc_utf8(buf);
      return buf;
    }
    if (check.yes) return in;
    buf = una::norm::to_nfc_utf8(in);
    return buf;
  }
  if (hasUpper) {
    buf.assign(in);
    for (char& c : buf) {
      if (c >= 'A' && c <= 'Z') c = (char) (c + ('a' - 'A'));
    }
    return buf;
  }
  return in;  // pure lowercase ASCII
}

// --- ASCII fast path for the fused standard tokenizer -----------------------
//
// uni-algo's UAX#29 walk is the dominant cost of the standard chain (~3/4 of
// tokenization time on a book corpus), and it runs codepoint-by-codepoint
// regardless of how ASCII the input is. The fast path
// below reproduces UAX#29 word breaking *exactly* on ASCII with a byte-class
// table, so pure-ASCII stretches never touch the library. Equivalence with the
// composed unicode_word + nfkc_cf chain is asserted in AnalysisTest (targeted
// cases, the WordBreakTest corpus, and a randomized mixed corpus).
//
// Byte classes (from WordBreakProperty.txt, ASCII range):
//   word chars: letters (ALetter), digits (Numeric), '_' (ExtendNumLet).
//     Adjacent word chars always join (WB5, WB8, WB9, WB10, WB13a, WB13b).
//   mid chars: join a kind-matched word char on each side (one lookahead):
//     ':' (MidLetter) letter-letter only (WB6/7); ',' ';' (MidNum) digit-digit
//     only (WB11/12); '.' '\'' (MidNumLet / Single_Quote) either same-kind
//     pair. Runs of mid chars never join ("x..y" breaks).
//   everything else breaks. (TAB is WB class Other, not whitespace, but Other
//   breaks on both sides exactly like whitespace, so word output is the same.)
// A run with no letter or digit (a bare "_") is not a word: word_only filters
// segments by their max word-break property and ExtendNumLet < Numeric.
enum AsciiWb : unsigned char {
  WB_BRK = 0, WB_LET, WB_NUM, WB_ENL, WB_MID_LET, WB_MID_NUM, WB_MID_BOTH
};

struct AsciiWbTable {
  unsigned char cls[256];
  constexpr AsciiWbTable() : cls() {
    for (int c = 'a'; c <= 'z'; c++) cls[c] = WB_LET;
    for (int c = 'A'; c <= 'Z'; c++) cls[c] = WB_LET;
    for (int c = '0'; c <= '9'; c++) cls[c] = WB_NUM;
    cls[(unsigned char) '_'] = WB_ENL;
    cls[(unsigned char) ':'] = WB_MID_LET;
    cls[(unsigned char) ','] = WB_MID_NUM;
    cls[(unsigned char) ';'] = WB_MID_NUM;
    cls[(unsigned char) '.'] = WB_MID_BOTH;
    cls[(unsigned char) '\''] = WB_MID_BOTH;
    // high bytes stay WB_BRK; they never reach the DFA (a region containing
    // one is routed to uni-algo instead)
  }
};
constexpr AsciiWbTable wbTable;

inline bool isWbWord(unsigned char cls) { return cls >= WB_LET && cls <= WB_ENL; }

inline bool isAscii(std::string_view s) {
  return ascii::findNonAscii(s.data(), s.size()) == s.size();
}

inline bool isAsciiWs(unsigned char c) {
  return c == ' ' || (c >= '\t' && c <= '\r');  // \t \n \v \f \r
}

// The fused default tokenizer: UAX#29 word segmentation with per-token NFKC_CF
// applied in the same stage. Equivalent to unicode_word + nfkc_cf (asserted in
// AnalysisTest) but without the extra filter hop / redundant ASCII scan. When
// the chain continues with `fold` (the _t chain), that is fused in too, but
// only as an equivalent shortcut: fold stays a separate, optional component
// (lossy, language-dependent; it is what distinguishes _t from _un).
//
// Hot-path structure: reset() copies the value into `lowered`, ASCII-lowercasing
// in one bulk SIMD pass (ASCII byte ops cannot corrupt multibyte UTF-8
// sequences, and pre-lowercasing ASCII never changes NFKC_CF output since
// casefold subsumes it). The scan then carves the value into regions delimited
// at *safe splits*: an ASCII whitespace byte followed by an ASCII byte (or end).
// ASCII whitespace always hard-breaks words (WSegSpace/CR/LF/Newline classes;
// TAB is Other, which also breaks both sides), and the only UAX#29 rule that
// reaches across a character is WB4 gluing Extend/Format/ZWJ onto it - all
// non-ASCII, ruled out by the followed-by-ASCII condition. So each region
// segments independently and boundaries match whole-value segmentation.
// Regions are found from the non-ASCII bytes, not the whitespace: a SIMD scan
// locates the next high byte, everything up to the last safe split before it
// is one pure-ASCII region (usually many words) for the byte-class DFA above,
// with no per-token fold work at all (already lowercased, NFKC is a no-op on
// ASCII), and the region holding the high byte, up to the next safe split
// after it, goes through uni-algo word_only + per-token NFKC_CF (+ fold), the
// fully conformant path.
//
// Tokens point into `lowered` (or the NFKC / fold scratch buffers): the
// producer's private reusable buffers, per the borrow contract.
class StandardTokenizer : public Tokenizer {
  using View = decltype(una::views::word_only::utf8(std::declval<std::string_view>()));
  using Iter = decltype(std::declval<View&>().begin());
  std::string lowered;           // ASCII-lowercased copy of the current value
  const char* loweredBase = nullptr;  // = lowered.data(), cached so offset math avoids reloading it
  const char* cur = nullptr;     // region scan cursor into `lowered`
  const char* valEnd = nullptr;
  const char* nextHigh = nullptr;  // first non-ASCII byte at or after cur once refreshed; valEnd if none
  const char* dfaCur = nullptr;  // active pure-ASCII region: [dfaCur, dfaEnd)
  const char* dfaEnd = nullptr;
  std::optional<View> view;      // active non-ASCII region: word_only over it
  Iter it{};
  std::string buf;               // NFKC_CF scratch for the current token
  std::string foldBuf;           // accent-fold scratch for the current token
  const bool foldAccents;        // fused `fold` filter (see Analyzer::fusedFold)

public:
  explicit StandardTokenizer(bool foldAccents) : foldAccents(foldAccents) {}

  void reset() override {
    size_t n = (size_t) (end_ - start_);
    const char* src = start_;
    lowered.resize_and_overwrite(n, [src](char* dst, size_t len) {
      ascii::lowerCopy(dst, src, len);
      return len;
    });
    loweredBase = lowered.data();
    cur = loweredBase;
    valEnd = cur + lowered.size();
    nextHigh = cur + ascii::findNonAscii(cur, n);
    dfaCur = dfaEnd = nullptr;
    view.reset();
  }

  // The fused nfkc_cf half applies to a bare term; the segmentation half does
  // not (see TokenStream::normalizeTerm).
  void normalizeTerm(std::string& term) override {
    std::string scratch;
    std::string_view folded = applyNfkcCf(term, scratch);
    if (folded.data() != term.data()) term.assign(folded);
    if (foldAccents && !isAscii(term)) term = una::norm::to_unaccent_utf8(term);
  }

  bool incrementToken() override {
    token.clear();
    for (;;) {
      // Drain the active region first. The ASCII DFA is the hot path and stays
      // inline; the uni-algo drain and the region dispatch are out of line.
      if (dfaCur != dfaEnd) {
        if (asciiNext()) return true;
      } else if (view) {
        if (viewNext()) return true;
      } else if (!nextRegion()) {
        return false;
      }
    }
  }

private:
  // Emit the next word from the active uni-algo region; false (and the view
  // released) when it is exhausted.
  [[gnu::noinline]] bool viewNext() {
    while (it != view->end()) {
      // Capture the source span before NFKC_CF repoints token.text at `buf`.
      // `lowered` is a length-preserving ASCII-lowercased copy, so byte
      // offsets into it equal byte offsets into the original value.
      std::string_view w = *it;
      token.setOffset((int) (w.data() - loweredBase),
                      (int) (w.data() + w.size() - loweredBase));
      token.text = applyNfkcCf(w, buf);
      ++it;
      if (token.text.empty()) continue;  // invisible words consume no position
      // Fused `fold`, exactly as AccentFoldFilter applies it after nfkc_cf
      // (including keeping a token that folds to empty). DFA tokens are
      // ASCII, which fold leaves alone, so only this path pays for it.
      if (foldAccents && !isAscii(token.text)) {
        foldBuf = una::norm::to_unaccent_utf8(token.text);
        token.text = foldBuf;
      }
      return true;
    }
    view.reset();
    return false;
  }

  // Start the next region at the cursor: an ASCII span for the DFA or a
  // non-ASCII span for uni-algo. False at the end of the value.
  [[gnu::noinline]] bool nextRegion() {
    // skip ASCII whitespace to the next region start
    while (cur < valEnd && isAsciiWs((unsigned char) *cur)) cur++;
    if (cur >= valEnd) return false;
    // Find the next non-ASCII byte (SIMD scan, amortized: it is only
    // recomputed once the cursor passes it). Everything before the last safe
    // split ahead of it is pure ASCII and goes to the DFA as one region; the
    // DFA's output does not depend on where regions split as long as they
    // split at whitespace.
    if (nextHigh < cur) nextHigh = cur + ascii::findNonAscii(cur, (size_t) (valEnd - cur));
    if (nextHigh == valEnd) {
      dfaCur = cur;
      dfaEnd = valEnd;
      cur = valEnd;
      return true;
    }
    // Last safe split before nextHigh: a whitespace byte at or before
    // nextHigh - 2, so the byte after it is ASCII.
    ptrdiff_t split = (nextHigh - cur) - 2;
    while (split >= 0 && !isAsciiWs((unsigned char) cur[split])) split--;
    if (split >= 0) {
      dfaCur = cur;
      dfaEnd = cur + split;
      cur += split;
      return true;
    }
    // The region holding nextHigh extends to the next safe split after it
    // and is segmented by uni-algo, the fully conformant path.
    const char* rs = cur;
    const char* p = nextHigh;
    while (p < valEnd) {
      if (isAsciiWs((unsigned char) *p) && (p + 1 == valEnd || ((unsigned char) p[1] & 0x80) == 0)) break;
      p++;
    }
    cur = p;
    view.emplace(una::views::word_only::utf8(std::string_view(rs, (size_t) (p - rs))));
    it = view->begin();
    return true;
  }

  // Emit the next word from the active ASCII region; false when exhausted.
  bool asciiNext() {
    const char* s = dfaCur;
    const char* e = dfaEnd;
    for (;;) {
      // skip to the next word char
      while (s < e && !isWbWord(wbTable.cls[(unsigned char) *s])) s++;
      if (s >= e) {
        dfaCur = dfaEnd;
        return false;
      }
      const char* w = s;
      bool sawLetNum = false;
      for (;;) {
        unsigned char k;
        while (s < e && isWbWord(k = wbTable.cls[(unsigned char) *s])) {
          sawLetNum |= (k <= WB_NUM);  // LET or NUM
          s++;
        }
        // a mid char continues the word iff bracketed by kind-matched word
        // chars (one-char lookahead; at region end a mid cannot join)
        if (s + 1 < e) {
          unsigned char m = wbTable.cls[(unsigned char) *s];
          if (m >= WB_MID_LET) {
            unsigned char prev = wbTable.cls[(unsigned char) s[-1]];
            unsigned char next = wbTable.cls[(unsigned char) s[1]];
            bool join = (m == WB_MID_BOTH) ? (prev == next && (prev == WB_LET || prev == WB_NUM))
                      : (m == WB_MID_LET)  ? (prev == WB_LET && next == WB_LET)
                                           : (prev == WB_NUM && next == WB_NUM);
            if (join) {
              s += 2;
              continue;
            }
          }
        }
        break;
      }
      if (sawLetNum) {
        token.text = std::string_view(w, (size_t) (s - w));
        token.setOffset((int) (w - loweredBase), (int) (s - loweredBase));
        dfaCur = s;
        return true;
      }
      // word-char run without any letter/digit (a bare "_"): not a word
    }
  }
};

// NFKC_CF as a standalone filter (for composing onto a non-fused tokenizer).
class NfkcCasefoldFilter : public TokenFilter {
  std::string buf;  // reusable output buffer; holds rewritten token bytes

public:
  NfkcCasefoldFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    // A dropped (all-ignorable) token takes no position of its own, but any
    // gap an upstream stage put before it still applies to the next token.
    int gap = 0;
    while (source().incrementToken()) {
      token.text = applyNfkcCf(token.text, buf);
      if (!token.text.empty()) {
        token.positionIncrement += gap;
        return true;
      }
      gap += token.positionIncrement - 1;
    }
    return false;
  }

  void normalizeTerm(std::string& term) override {
    TokenFilter::normalizeTerm(term);
    std::string scratch;
    std::string_view folded = applyNfkcCf(term, scratch);
    if (folded.data() != term.data()) term.assign(folded);
  }
};

// Accent/diacritic folding: strips combining marks so an accented letter matches
// its bare form (cafe-with-accent -> cafe, naive-with-diaeresis -> naive). Applied
// after nfkc_cf. Lossy and language-dependent (it conflates letters that are
// distinct in e.g. Swedish/Spanish), hence a separate opt-in. Accents are always
// non-ASCII in UTF-8, so pure-ASCII tokens pass through untouched (zero copy).
class AccentFoldFilter : public TokenFilter {
  std::string buf;  // reusable output buffer

public:
  AccentFoldFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;

    if (!isAscii(token.text)) {
      buf = una::norm::to_unaccent_utf8(token.text);
      token.text = buf;
    }
    return true;
  }

  void normalizeTerm(std::string& term) override {
    TokenFilter::normalizeTerm(term);
    for (unsigned char c : term) {
      if (c & 0x80) {
        term = una::norm::to_unaccent_utf8(term);
        return;
      }
    }
  }
};

}  // namespace

std::string_view applyLowercase(std::string_view in, std::string& buf) {
  size_t firstUpper = in.size();
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char) in[i];
    if (c & 0x80) {
      // Full Unicode lowercase (default un-tailored casing); also lowercases
      // any ASCII uppercase preceding this byte.
      buf = una::cases::to_lowercase_utf8(in);
      return buf;
    }
    if (c >= 'A' && c <= 'Z' && firstUpper == in.size()) firstUpper = i;
  }
  if (firstUpper == in.size()) return in;  // pure lowercase ASCII: zero copy
  buf.assign(in);
  for (size_t i = firstUpper; i < buf.size(); i++) {
    char c = buf[i];
    if (c >= 'A' && c <= 'Z') buf[i] = (char) (c + ('a' - 'A'));
  }
  return buf;
}

std::unique_ptr<Tokenizer> makeUnicodeWordTokenizer() {
  return std::make_unique<UnicodeWordTokenizer>();
}

std::unique_ptr<Tokenizer> makeStandardTokenizer(bool foldAccents) {
  return std::make_unique<StandardTokenizer>(foldAccents);
}

std::unique_ptr<TokenStream> makeNfkcCasefoldFilter(std::unique_ptr<TokenStream> source) {
  return std::make_unique<NfkcCasefoldFilter>(std::move(source));
}

std::unique_ptr<TokenStream> makeAccentFoldFilter(std::unique_ptr<TokenStream> source) {
  return std::make_unique<AccentFoldFilter>(std::move(source));
}

// ---------------------------------------------------------------------------
// Component registry

namespace {

std::unique_ptr<Tokenizer> makeWhitespaceTokenizer() { return std::make_unique<WhitespaceTokenizer>(); }
std::unique_ptr<Tokenizer> makeKeywordTokenizer() { return std::make_unique<KeywordTokenizer>(); }
std::unique_ptr<TokenStream> makeLowercaseFilter(std::unique_ptr<TokenStream> source) {
  return std::make_unique<LowercaseFilter>(std::move(source));
}

class EnglishPossessiveFilter : public TokenFilter {
public:
  EnglishPossessiveFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    token.text = removeEnglishPossessive(token.text);
    return true;
  }

  // Like stemming, possessive removal is analysis-only, not normalization.
};

std::unique_ptr<TokenStream> makeEnglishPossessiveFilter(std::unique_ptr<TokenStream> source) {
  return std::make_unique<EnglishPossessiveFilter>(std::move(source));
}

template <bool Possessive>
class KStemFilter : public TokenFilter {
  KStemmer stemmer;
public:
  KStemFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    token.text = stemmer.stem<Possessive>(token.text);
    return true;
  }

  // Inherit normalization forwarding: multiterm input is folded, never stemmed.
};

std::unique_ptr<const TokenFilterFactory> kstemFilter(const api::AnalyzerComponent& def) {
  struct Factory : TokenFilterFactory {
    bool possessive = true;

    std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
      if (possessive) return std::make_unique<KStemFilter<true>>(std::move(source));
      return std::make_unique<KStemFilter<false>>(std::move(source));
    }
  };
  auto factory = std::make_unique<Factory>();
  factory->name = std::string(def.name);
  factory->normalizing = false;
  for (const auto& [key, value] : def.params) {
    if (key != "possessive") {
      throw std::invalid_argument("filter 'kstem': unknown parameter '" + std::string(key) +
                                  "'; valid parameters: possessive");
    }
    auto* flag = std::get_if<bool>(&value->kind);
    if (!flag) throw std::invalid_argument("filter 'kstem': parameter 'possessive' must be a boolean");
    factory->possessive = *flag;
  }
  return factory;
}

// Byte length of the UTF-8 sequence starting at `i`: the lead byte plus its
// continuation bytes. Ill-formed input still advances by at least one byte.
size_t nextCodepoint(std::string_view text, size_t i) {
  i++;
  while (i < text.size() && ((unsigned char) text[i] & 0xC0) == 0x80) i++;
  return i;
}

// Emits the leading min_gram..max_gram code points of each token, all at the
// token's position and with its source offsets. A token shorter than
// min_gram is dropped, and its position increment carries to the next token
// so later positions match the unfiltered chain.
class EdgeNGramFilter : public TokenFilter {
  const int minGram;
  const int maxGram;
  std::string_view word;  // the current source token; the source is not pulled until its grams are out
  size_t gramEnd = 0;     // byte end of the last emitted gram
  int grams = 0;          // code points in the last emitted gram; 0 = no current token
public:
  EdgeNGramFilter(std::unique_ptr<TokenStream> source, int minGram, int maxGram)
      : TokenFilter(std::move(source)), minGram(minGram), maxGram(maxGram) {}

  void reset() override {
    TokenFilter::reset();
    grams = 0;
  }

  bool incrementToken() override {
    if (grams > 0 && grams < maxGram && gramEnd < word.size()) {
      gramEnd = nextCodepoint(word, gramEnd);
      grams++;
      token.text = word.substr(0, gramEnd);
      token.positionIncrement = 0;
      return true;
    }
    int gap = 0;
    while (source().incrementToken()) {
      word = token.text;
      gramEnd = 0;
      grams = 0;
      while (grams < minGram && gramEnd < word.size()) {
        gramEnd = nextCodepoint(word, gramEnd);
        grams++;
      }
      if (grams < minGram) {
        gap += token.positionIncrement;
        continue;
      }
      token.text = word.substr(0, gramEnd);
      token.positionIncrement += gap;
      return true;
    }
    grams = 0;
    return false;
  }

  // Grams are an indexing expansion; multiterm input is folded, never split.
};

// Joins runs of min_shingle_size..max_shingle_size consecutive tokens with
// token_separator, optionally emitting the single tokens too. Every output
// sits at the position of its first token, spanning the source offsets of
// its first through last token. Tokens join only at consecutive positions: a
// position gap (or a stacked token) starts a new run, and runs never cross
// values. Shingles from one start position come out before the next start.
class ShingleFilter : public TokenFilter {
  struct Word {
    std::string text;
    int position;
    int startOffset;
    int endOffset;
  };
  const int minSize;
  const int maxSize;
  const bool unigrams;
  const std::string separator;
  std::deque<Word> window;      // consecutive words; window[0] is the current start
  std::optional<Word> pending;  // the word after a position break; starts the next run
  bool exhausted = false;
  int position = -1;            // position of the last word pulled from the source
  int emitted = -1;             // position of the last token emitted
  int size = 0;                 // words in the next output from window[0]; 0 = not started
  std::string out;              // the emitted token's bytes

  void pull() {
    if (!source().incrementToken()) {
      exhausted = true;
      return;
    }
    position += token.positionIncrement;
    Word word{std::string(token.text), position, token.startOffset, token.endOffset};
    if (!window.empty() && position != window.back().position + 1) {
      pending = std::move(word);
    } else {
      window.push_back(std::move(word));
    }
  }

public:
  ShingleFilter(std::unique_ptr<TokenStream> source, int minSize, int maxSize, bool unigrams,
                std::string separator)
      : TokenFilter(std::move(source)), minSize(minSize), maxSize(maxSize), unigrams(unigrams),
        separator(std::move(separator)) {}

  void reset() override {
    TokenFilter::reset();
    window.clear();
    pending.reset();
    exhausted = false;
    position = -1;
    emitted = -1;
    size = 0;
  }

  bool incrementToken() override {
    for (;;) {
      if (window.empty()) {
        if (pending) {
          window.push_back(std::move(*pending));
          pending.reset();
        } else if (!exhausted) {
          pull();
        }
        if (window.empty()) return false;
      }
      while (window.size() < (size_t) maxSize && !pending && !exhausted) pull();
      if (size == 0) size = unigrams ? 1 : minSize;
      if (size <= (int) window.size()) {
        const Word& first = window[0];
        const Word& last = window[(size_t) size - 1];
        out = first.text;
        for (int i = 1; i < size; i++) {
          out += separator;
          out += window[(size_t) i].text;
        }
        token.text = out;
        token.positionIncrement = first.position - emitted;
        token.setOffset(first.startOffset, last.endOffset);
        emitted = first.position;
        size = size == 1 ? minSize : size + 1;
        return true;
      }
      window.pop_front();
      size = 0;
    }
  }

  // Shingles are an indexing expansion; multiterm input is folded, never joined.
};

// An integer component parameter in [lo, hi]. JSON numbers may arrive as
// integral doubles.
int intParam(std::string_view component, std::string_view key, const api::Val& value, int lo, int hi) {
  std::optional<int64_t> n;
  if (auto* i = std::get_if<int64_t>(&value.kind)) n = *i;
  else if (auto* d = std::get_if<double>(&value.kind); d && *d >= lo && *d <= hi && *d == (double) (int) *d) {
    n = (int64_t) *d;
  }
  if (!n || *n < lo || *n > hi) {
    throw std::invalid_argument("filter '" + std::string(component) + "': parameter '" + std::string(key) +
                                "' must be an integer from " + std::to_string(lo) + " to " +
                                std::to_string(hi));
  }
  return (int) *n;
}

// Shared by the edge_ngram and shingle factories so Analyzer::compile can read
// their parameters back into Analyzer::Accel.
struct AccelFactory : TokenFilterFactory {
  Analyzer::Accel accel;
};

std::unique_ptr<const TokenFilterFactory> edgeNGramFilter(const api::AnalyzerComponent& def) {
  struct Factory : AccelFactory {
    std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
      return std::make_unique<EdgeNGramFilter>(std::move(source), accel.minSize, accel.maxSize);
    }
  };
  auto factory = std::make_unique<Factory>();
  factory->name = std::string(def.name);
  factory->stateful = true;
  factory->accel.kind = Analyzer::Accel::Kind::PREFIXES;
  factory->accel.minSize = 2;
  factory->accel.maxSize = 5;
  for (const auto& [key, value] : def.params) {
    if (key == "min_gram") factory->accel.minSize = intParam(def.name, key, *value, 1, 50);
    else if (key == "max_gram") factory->accel.maxSize = intParam(def.name, key, *value, 1, 50);
    else {
      throw std::invalid_argument("filter 'edge_ngram': unknown parameter '" + std::string(key) +
                                  "'; valid parameters: min_gram, max_gram");
    }
  }
  if (factory->accel.minSize > factory->accel.maxSize) {
    throw std::invalid_argument("filter 'edge_ngram': min_gram must not exceed max_gram");
  }
  return factory;
}

std::unique_ptr<const TokenFilterFactory> shingleFilter(const api::AnalyzerComponent& def) {
  struct Factory : AccelFactory {
    std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
      return std::make_unique<ShingleFilter>(std::move(source), accel.minSize, accel.maxSize,
                                             accel.unigrams, accel.separator);
    }
  };
  auto factory = std::make_unique<Factory>();
  factory->name = std::string(def.name);
  factory->stateful = true;
  factory->accel.kind = Analyzer::Accel::Kind::SHINGLES;
  factory->accel.minSize = 2;
  factory->accel.maxSize = 2;
  factory->accel.separator = " ";
  for (const auto& [key, value] : def.params) {
    if (key == "min_shingle_size") {
      factory->accel.minSize = intParam(def.name, key, *value, 2, 5);
    } else if (key == "max_shingle_size") {
      factory->accel.maxSize = intParam(def.name, key, *value, 2, 5);
    } else if (key == "output_unigrams") {
      auto* flag = std::get_if<bool>(&value->kind);
      if (!flag) throw std::invalid_argument("filter 'shingle': parameter 'output_unigrams' must be a boolean");
      factory->accel.unigrams = *flag;
    } else if (key == "token_separator") {
      auto* text = std::get_if<std::string_view>(&value->kind);
      if (!text) throw std::invalid_argument("filter 'shingle': parameter 'token_separator' must be a string");
      factory->accel.separator = std::string(*text);
    } else {
      throw std::invalid_argument("filter 'shingle': unknown parameter '" + std::string(key) +
                                  "'; valid parameters: min_shingle_size, max_shingle_size, "
                                  "output_unigrams, token_separator");
    }
  }
  if (factory->accel.minSize > factory->accel.maxSize) {
    throw std::invalid_argument("filter 'shingle': min_shingle_size must not exceed max_shingle_size");
  }
  return factory;
}

void requireNoParams(const api::AnalyzerComponent& def, std::string_view kind) {
  if (def.params.empty()) return;
  std::string got;
  for (const auto& [key, val] : def.params) {
    if (!got.empty()) got += ", ";
    got += key;
  }
  throw std::invalid_argument(std::string(kind) + " '" + std::string(def.name) +
                              "' takes no parameters (got: " + got + ")");
}

// A parameterless tokenizer / filter: any params are rejected and create()
// forwards to the plain constructor function Make.  Stateful marks a stage
// that buffers across tokens (see TokenStreamFactory::stateful).
template <std::unique_ptr<Tokenizer> (*Make)(), bool Stateful = false>
std::unique_ptr<const TokenizerFactory> plainTokenizer(const api::AnalyzerComponent& def) {
  requireNoParams(def, "tokenizer");
  struct Factory : TokenizerFactory {
    std::unique_ptr<Tokenizer> create() const override { return Make(); }
  };
  auto factory = std::make_unique<Factory>();
  factory->name = std::string(def.name);
  factory->stateful = Stateful;
  return factory;
}

template <std::unique_ptr<TokenStream> (*Make)(std::unique_ptr<TokenStream>),
          bool Normalizing = true, bool Stateful = false>
std::unique_ptr<const TokenFilterFactory> plainFilter(const api::AnalyzerComponent& def) {
  requireNoParams(def, "filter");
  struct Factory : TokenFilterFactory {
    std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
      return Make(std::move(source));
    }
  };
  auto factory = std::make_unique<Factory>();
  factory->name = std::string(def.name);
  factory->stateful = Stateful;
  factory->normalizing = Normalizing;
  return factory;
}

struct TokenizerEntry {
  std::string_view name;
  std::unique_ptr<const TokenizerFactory> (*make)(const api::AnalyzerComponent&);
};
struct FilterEntry {
  std::string_view name;
  std::unique_ptr<const TokenFilterFactory> (*make)(const api::AnalyzerComponent&);
};

// The registries: adding a component is one line here plus its factory (a
// param-taking one reads its Val params in the maker).  unicode_word is
// stateful: it carries a segmentation cursor and rebuilds it in reset().
constexpr TokenizerEntry TOKENIZERS[] = {
  {"whitespace", plainTokenizer<makeWhitespaceTokenizer>},
  {"keyword", plainTokenizer<makeKeywordTokenizer>},
  {"unicode_word", plainTokenizer<makeUnicodeWordTokenizer, true>},
};
constexpr FilterEntry FILTERS[] = {
  {"lowercase", plainFilter<makeLowercaseFilter>},
  {"nfkc_cf", plainFilter<makeNfkcCasefoldFilter>},
  {"fold", plainFilter<makeAccentFoldFilter>},
  {"english_possessive", plainFilter<makeEnglishPossessiveFilter, false>},
  {"kstem", kstemFilter},
  {"edge_ngram", edgeNGramFilter},
  {"shingle", shingleFilter},
};

template <class Entry, size_t N>
std::string joinNames(const Entry (&table)[N]) {
  std::string out;
  for (const auto& entry : table) {
    if (!out.empty()) out += ", ";
    out += entry.name;
  }
  return out;
}

}  // namespace

std::unique_ptr<const TokenizerFactory> makeTokenizerFactory(const api::AnalyzerComponent& def) {
  if (def.name.empty()) {
    throw std::invalid_argument("tokenizer component has no name; valid tokenizers: " + joinNames(TOKENIZERS));
  }
  for (const auto& entry : TOKENIZERS) {
    if (entry.name == def.name) return entry.make(def);
  }
  throw std::invalid_argument("unknown tokenizer '" + std::string(def.name) +
                              "'; valid tokenizers: " + joinNames(TOKENIZERS));
}

std::unique_ptr<const TokenFilterFactory> makeTokenFilterFactory(const api::AnalyzerComponent& def) {
  if (def.name.empty()) {
    throw std::invalid_argument("filter component has no name; valid filters: " + joinNames(FILTERS));
  }
  for (const auto& entry : FILTERS) {
    if (entry.name == def.name) return entry.make(def);
  }
  throw std::invalid_argument("unknown filter '" + std::string(def.name) +
                              "'; valid filters: " + joinNames(FILTERS));
}

namespace {

std::string canonicalJson(const api::AnalyzerDef& def) {
  api::AnalyzerDef effective = def;
  if (!effective.tokenizer) effective.tokenizer.emplace().name = "whitespace";
  std::string json;
  if (!api::write_json(effective, json)) throw std::runtime_error("Cannot serialize analyzer");
  // Sort parameter objects recursively; filter order remains semantic.
  glz::generic_sorted_u64 value;
  if (glz::read_json(value, json)) throw std::runtime_error("Cannot canonicalize analyzer");
  // Give the empty chain the same representation everywhere.
  if (def.filters.empty()) value["filters"] = glz::generic_sorted_u64::array_t{};
  if (glz::write_json(value, json)) throw std::runtime_error("Cannot serialize canonical analyzer");
  return json;
}

}  // namespace

std::shared_ptr<const Analyzer> Analyzer::compile(const api::AnalyzerDef& def) {
  api::AnalyzerComponent whitespace;
  whitespace.name = "whitespace";
  auto tokenizer = makeTokenizerFactory(def.tokenizer.has_value() ? *def.tokenizer : whitespace);
  std::vector<std::unique_ptr<const TokenFilterFactory>> filters;
  filters.reserve(def.filters.size());
  for (const auto& filter : def.filters) {
    filters.push_back(makeTokenFilterFactory(filter));
  }
  Accel accel;
  if (auto* last = filters.empty() ? nullptr : dynamic_cast<const AccelFactory*>(filters.back().get())) {
    accel = last->accel;
    // Shingles join consecutive positions only, so a base with stacked
    // edge_ngram tokens would lose pairs a phrase on the base still matches.
    bool stacked = std::any_of(filters.begin(), filters.end() - 1,
                               [](const auto& f) { return f->name == "edge_ngram"; });
    if (accel.kind == Accel::Kind::SHINGLES && stacked) accel.kind = Accel::Kind::NONE;
    api::AnalyzerDef base = def;
    base.filters = def.filters.first(def.filters.size() - 1);
    accel.baseCanonical = canonicalJson(base);
  }
  return std::shared_ptr<const Analyzer>(new Analyzer(std::move(tokenizer), std::move(filters),
                                                      canonicalJson(def), std::move(accel)));
}

Analyzer::Analyzer(std::unique_ptr<const TokenizerFactory> tok,
                   std::vector<std::unique_ptr<const TokenFilterFactory>> fs, std::string canonical,
                   Accel accel)
    : canonical(std::move(canonical)), accel(std::move(accel)), tokenizer(std::move(tok)),
      filters(std::move(fs)),
      fusedHead(tokenizer->name == "unicode_word" && !filters.empty() && filters[0]->name == "nfkc_cf"),
      fusedFold(fusedHead && filters.size() > 1 && filters[1]->name == "fold"),
      stateful(tokenizer->stateful ||
               std::any_of(filters.begin(), filters.end(), [](const auto& f) { return f->stateful; })) {}

std::unique_ptr<TokenChain> Analyzer::createChain() const {
  std::unique_ptr<Tokenizer> head = fusedHead ? makeStandardTokenizer(fusedFold) : tokenizer->create();
  auto& headRef = *head;
  std::unique_ptr<TokenStream> tail = std::move(head);
  for (size_t i = fusedFold ? 2 : fusedHead ? 1 : 0; i < filters.size(); i++) {
    tail = filters[i]->create(std::move(tail));
  }
  return std::make_unique<TokenChain>(headRef, std::move(tail), stateful);
}

}  // namespace luxir
