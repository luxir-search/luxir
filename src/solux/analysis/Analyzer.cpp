#include "solux/analysis/Analyzer.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <uni_algo/case.h>
#include <uni_algo/norm.h>
#include <uni_algo/ranges_word.h>

// The uni-algo dependency is confined to this translation unit; the rest of the
// codebase sees only the factory functions declared in Analyzer.h. uni-algo is
// pinned/vendored (Unicode 15.1.0) - its version is part of the index format.

namespace solux {

namespace {

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

// Apply NFKC_CF (toNFKC_Casefold) to `in`, using `buf` as scratch only when a
// rewrite is needed. Returns a view of the result - which is `in` itself for the
// common pure-lowercase-ASCII case (zero copy). Tiered fast path:
//   * pure lowercase ASCII -> return `in` unchanged
//   * ASCII with uppercase -> plain ASCII lowercase into buf (NFKC is a no-op on
//     ASCII, casefold on ASCII is lowercase)
//   * any non-ASCII byte    -> full NFKC_CF via uni-algo
// (toNFKC_Casefold is approximated as casefold-then-NFKC; a single-pass NFKC_CF
// mapping is a later optimization. Conformance is gated in AnalysisTest.)
std::string_view applyNfkcCf(std::string_view in, std::string& buf) {
  bool hasUpper = false, hasNonAscii = false;
  for (unsigned char c : in) {
    if (c & 0x80) { hasNonAscii = true; break; }
    if (c >= 'A' && c <= 'Z') hasUpper = true;
  }
  if (hasNonAscii) {
    buf = una::norm::to_nfkc_utf8(una::cases::to_casefold_utf8(in));
    // NFKC can surface fresh uppercase (e.g. U+03D3 -> U+038E), so one pass is
    // not the NFKC_CF fixpoint; re-fold until stable (typically zero iterations:
    // casefold of already-folded text is the identity, so the loop is one
    // compare in the common case).
    for (;;) {
      std::string refolded = una::cases::to_casefold_utf8(buf);
      if (refolded == buf) break;
      buf = una::norm::to_nfkc_utf8(refolded);
    }
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

inline bool isAsciiWs(unsigned char c) {
  return c == ' ' || (c >= '\t' && c <= '\r');  // \t \n \v \f \r
}

// The fused default tokenizer: UAX#29 word segmentation with per-token NFKC_CF
// applied in the same stage. Equivalent to unicode_word + nfkc_cf (asserted in
// AnalysisTest) but without the extra filter hop / redundant ASCII scan. Accent
// folding is intentionally NOT fused in - it stays the separate, optional `fold`
// filter (lossy, language-dependent; it is what distinguishes _t from _wl).
//
// Hot-path structure: reset() copies the value into `lowered`, ASCII-lowercasing
// in one bulk pass (vectorizable; ASCII byte ops cannot corrupt multibyte UTF-8
// sequences, and pre-lowercasing ASCII never changes NFKC_CF output since
// casefold subsumes it). The scan then carves the value into regions delimited
// at *safe splits*: an ASCII whitespace byte followed by an ASCII byte (or end).
// ASCII whitespace always hard-breaks words (WSegSpace/CR/LF/Newline classes;
// TAB is Other, which also breaks both sides), and the only UAX#29 rule that
// reaches across a character is WB4 gluing Extend/Format/ZWJ onto it - all
// non-ASCII, ruled out by the followed-by-ASCII condition. So each region
// segments independently and boundaries match whole-value segmentation. A
// pure-ASCII region (the common case) is tokenized by the byte-class DFA above
// with no per-token fold work at all (already lowercased, NFKC is a no-op on
// ASCII); a region containing a high byte goes through uni-algo word_only +
// per-token NFKC_CF, the fully conformant path.
//
// Tokens point into `lowered` (or the NFKC scratch `buf`): the producer's
// private reusable buffers, per the borrow contract.
class StandardTokenizer : public Tokenizer {
  using View = decltype(una::views::word_only::utf8(std::declval<std::string_view>()));
  using Iter = decltype(std::declval<View&>().begin());
  std::string lowered;           // ASCII-lowercased copy of the current value
  const char* loweredBase = nullptr;  // = lowered.data(), cached so offset math avoids reloading it
  const char* cur = nullptr;     // region scan cursor into `lowered`
  const char* valEnd = nullptr;
  const char* dfaCur = nullptr;  // active pure-ASCII region: [dfaCur, dfaEnd)
  const char* dfaEnd = nullptr;
  std::optional<View> view;      // active non-ASCII region: word_only over it
  Iter it{};
  std::string buf;               // NFKC_CF scratch for the current token

public:
  void reset() override {
    lowered.assign(start_, (size_t) (end_ - start_));
    for (char& ch : lowered) {
      if ((unsigned char) (ch - 'A') < 26) ch = (char) (ch + ('a' - 'A'));
    }
    loweredBase = lowered.data();
    cur = loweredBase;
    valEnd = cur + lowered.size();
    dfaCur = dfaEnd = nullptr;
    view.reset();
  }

  bool incrementToken() override {
    token.clear();
    for (;;) {
      // drain the active region first
      if (dfaCur != dfaEnd) {
        if (asciiNext()) return true;
      } else if (view) {
        if (it != view->end()) {
          // Capture the source span before NFKC_CF repoints token.text at `buf`.
          // `lowered` is a length-preserving ASCII-lowercased copy, so byte
          // offsets into it equal byte offsets into the original value.
          std::string_view w = *it;
          token.setOffset((int) (w.data() - loweredBase),
                          (int) (w.data() + w.size() - loweredBase));
          token.text = applyNfkcCf(w, buf);
          ++it;
          return true;
        }
        view.reset();
      }
      // skip ASCII whitespace to the next region start
      while (cur < valEnd && isAsciiWs((unsigned char) *cur)) cur++;
      if (cur >= valEnd) return false;
      // region extends to the next safe split (see class comment); track
      // whether it is pure ASCII as we scan
      const char* rs = cur;
      const char* p = cur;
      bool high = false;
      while (p < valEnd) {
        unsigned char c = (unsigned char) *p;
        if (isAsciiWs(c) && (p + 1 == valEnd || ((unsigned char) p[1] & 0x80) == 0)) break;
        high |= (c & 0x80) != 0;
        p++;
      }
      cur = p;
      if (!high) {
        dfaCur = rs;
        dfaEnd = p;
      } else {
        view.emplace(una::views::word_only::utf8(std::string_view(rs, (size_t) (p - rs))));
        it = view->begin();
      }
    }
  }

private:
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
    if (!source().incrementToken()) return false;
    token.text = applyNfkcCf(token.text, buf);
    return true;
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

    std::string_view in = token.text;
    bool hasNonAscii = false;
    for (unsigned char c : in) {
      if (c & 0x80) { hasNonAscii = true; break; }
    }
    if (hasNonAscii) {
      buf = una::norm::to_unaccent_utf8(in);
      token.text = buf;
    }
    return true;
  }
};

}  // namespace

std::unique_ptr<Tokenizer> makeUnicodeWordTokenizer() {
  return std::make_unique<UnicodeWordTokenizer>();
}

std::unique_ptr<Tokenizer> makeStandardTokenizer() {
  return std::make_unique<StandardTokenizer>();
}

std::unique_ptr<TokenStream> makeNfkcCasefoldFilter(std::unique_ptr<TokenStream> source) {
  return std::make_unique<NfkcCasefoldFilter>(std::move(source));
}

std::unique_ptr<TokenStream> makeAccentFoldFilter(std::unique_ptr<TokenStream> source) {
  return std::make_unique<AccentFoldFilter>(std::move(source));
}

}  // namespace solux
