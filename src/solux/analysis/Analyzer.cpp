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

// NFKC_CF (toNFKC_Casefold): compatibility normalization fused with full case
// folding, applied per token into a reusable buffer. Tiered fast path:
//   * pure lowercase ASCII -> pass through, zero copy
//   * ASCII with uppercase -> plain ASCII lowercase into the buffer (NFKC is a
//     no-op on ASCII, casefold on ASCII is lowercase)
//   * any non-ASCII byte    -> full NFKC_CF via uni-algo
// (toNFKC_Casefold is approximated as casefold-then-NFKC; a single-pass NFKC_CF
// mapping is a later optimization. Conformance is gated in AnalysisTest.)
class NfkcCasefoldFilter : public TokenFilter {
  std::string buf;  // reusable output buffer; holds rewritten token bytes

public:
  NfkcCasefoldFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;

    std::string_view in = token.text;
    bool hasUpper = false, hasNonAscii = false;
    for (unsigned char c : in) {
      if (c & 0x80) { hasNonAscii = true; break; }
      if (c >= 'A' && c <= 'Z') hasUpper = true;
    }

    if (hasNonAscii) {
      buf = una::norm::to_nfkc_utf8(una::cases::to_casefold_utf8(in));
      token.text = buf;
    } else if (hasUpper) {
      buf.assign(in);
      for (char& c : buf) {
        if (c >= 'A' && c <= 'Z') c = (char) (c + ('a' - 'A'));
      }
      token.text = buf;
    }
    // else: pure lowercase ASCII - leave token.text pointing at the source bytes.
    return true;
  }
};

}  // namespace

std::unique_ptr<Tokenizer> makeUnicodeWordTokenizer() {
  return std::make_unique<UnicodeWordTokenizer>();
}

std::unique_ptr<TokenStream> makeNfkcCasefoldFilter(std::unique_ptr<TokenStream> source) {
  return std::make_unique<NfkcCasefoldFilter>(std::move(source));
}

}  // namespace solux
