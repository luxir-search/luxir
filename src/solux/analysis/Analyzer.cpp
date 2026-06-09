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
    token.text = *it;
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

// The fused default tokenizer: UAX#29 word segmentation with per-token NFKC_CF
// applied in the same stage. Equivalent to unicode_word + nfkc_cf (asserted in
// AnalysisTest) but without the extra filter hop / redundant ASCII scan. Accent
// folding is intentionally NOT fused in - it stays the separate, optional `fold`
// filter (lossy, language-dependent; it is what distinguishes _t from _wl).
class StandardTokenizer : public Tokenizer {
  using View = decltype(una::views::word_only::utf8(std::declval<std::string_view>()));
  using Iter = decltype(std::declval<View&>().begin());
  std::optional<View> view;
  Iter it{};
  std::string buf;  // NFKC_CF scratch for the current token

public:
  void reset() override {
    view.emplace(una::views::word_only::utf8(std::string_view(start_, (size_t) (end_ - start_))));
    it = view->begin();
  }

  bool incrementToken() override {
    token.clear();
    if (!view || it == view->end()) return false;
    std::string_view seg = *it;  // borrowed view into the source
    ++it;
    token.text = applyNfkcCf(seg, buf);
    return true;
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
