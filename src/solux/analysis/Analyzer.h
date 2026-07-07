#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cstring>
#include <memory>
#include "solux/util/solux_util.h"

namespace solux {

// A Token is a borrowed view, not an owner. `text` points at bytes that live in
// the source value, in a producing filter's private reusable buffer, or in a
// static dictionary. The Token just points.
//
// The contract every filter in a chain must obey:
//   * read-only  - never write the bytes at `text`. To "change the text", a
//                  rewriting filter copies into its own buffer and repoints
//                  `text` at it. This protects both the caller's source value
//                  and every upstream filter's output buffer (no aliasing).
//   * transient  - `text` is valid only for the current token. It is invalidated
//                  by the next incrementToken()/reset()/end() on the chain. A
//                  buffering filter (shingles, CJK bigram) that needs to retain a
//                  token past the current pull must copy the bytes out.
// `text` (a string_view) is itself an attribute: the only way to change the
// token's text is to reassign it. Positions/type/payload would join the same
// mutable bag.
class Token {
public:
  std::string_view text;
  int positionIncrement; // increment from the previous token's position
  int startOffset; // byte offset into original text buffer of current token
  int endOffset;   // end offset (exclusive) into the original text buffer

  void clear() {
    // Offsets are deliberately not reset here: every emit path sets them, so
    // zeroing first would just be a dead store on the hot path.
    positionIncrement = 1;
  }

  // Record the [start, end) source byte span. Only the head Tokenizer (which
  // still sees the source) sets this; downstream filters generally leave it alone so it
  // keeps pointing at the origin span even after they rewrite `text`.
  void setOffset(int start, int end) {
    startOffset = start;
    endOffset = end;
  }
};


// A pull-based stream of Tokens. The whole chain shares a single Token object
// (held by the head Tokenizer); each stage repoints its `text`/attributes in
// place. Lifecycle is reset() / incrementToken()* / end():
//   * reset()           - prepare to emit tokens for a fresh value. The head's
//                         input is supplied separately via Tokenizer::setValue.
//                         Defaults to no-op (and non-forwarding); only stateful
//                         (buffering) filters need to override it.
//   * incrementToken()  - advance to the next token, returning false at the end.
//   * end()             - reserved seam for reporting trailing stream state
//                         (final offset, trailing position increment). Not needed
//                         yet; will be added no-op-defaulted when the first
//                         feature that produces trailing state lands.
class TokenStream {
protected:
  Token &token;
public:
  TokenStream(Token &tok) : token(tok) {
  }

  virtual ~TokenStream() = default;

  Token &getToken() { return token; }

  // Prepare the stream to emit tokens for a fresh value. Default no-op: the
  // common whitespace+lowercase chain holds no cross-token state, so there is
  // nothing to reset (the head cursor is reset by setValue). Stateful filters
  // override to clear their buffers; see TokenFilter::reset for forwarding.
  virtual void reset() {}

  virtual bool incrementToken() = 0;

  // Normalize a single term's bytes IN PLACE the way this stage transforms
  // token text, without segmentation (the Lucene Analyzer::normalize
  // lineage): case/character folds apply, word-boundary decisions do not.
  // For multiterm query input - prefix and fuzzy terms (and term-range
  // endpoints when those land) never go through tokenization, but must still
  // fold the way indexed terms were folded or a capitalized prefix silently
  // matches nothing.  Default identity; rewriting filters (and tokenizers
  // with a fused fold) override.  See TokenFilter::normalizeTerm for
  // forwarding.  Cold path: per-call std::string allocation is fine.
  virtual void normalizeTerm(std::string& term) { unused(term); }
};

class Tokenizer : public TokenStream {
protected:
  // Private scan cursors. These move forward through the source as we carve out
  // tokens; each token is published as a string_view into [tokStart, start_).
  const char *start_ = nullptr;
  const char *end_ = nullptr;
  // Start of the current value. start_ advances as we scan, so token offsets are
  // measured against this fixed origin rather than the moving cursor.
  const char *base_ = nullptr;
  Token token;
public:
  Tokenizer() : TokenStream(token) {}

  // The value must outlive the analysis. It is treated as read-only: under the
  // borrow contract no stage writes through these bytes.
  void setValue(std::string_view val) {
    start_ = val.data();
    end_ = start_ + val.size();
    base_ = start_;
  }
};


class TokenFilter : public TokenStream {
  std::unique_ptr<TokenStream> tokSource;
public:
  TokenFilter(std::unique_ptr<TokenStream> source) : TokenStream(source->getToken()), tokSource(std::move(source)) {
  }

  TokenStream& source() { return *tokSource; }

  // Forward reset() source-ward so a reset propagates to every stage. This is
  // only invoked when the chain actually contains a stateful filter (see
  // TokenChain::reset); the common stateless chain skips it entirely.
  void reset() override { tokSource->reset(); }

  // Forward normalizeTerm source-ward so every stage sees the term in chain
  // order. A rewriting filter overrides, calls this first, then applies its
  // own fold.
  void normalizeTerm(std::string& term) override { tokSource->normalizeTerm(term); }
};

// Splits the source on ASCII whitespace, viewing the source bytes directly (no
// copy). Under the borrow contract the tokenizer never needs a writable buffer:
// each token is a string_view into the source, and any rewriting happens later
// in a filter's own buffer.
class WhitespaceTokenizer : public Tokenizer {

  // increments over a single whitespace char.
  // assumes ptr is less than end
  // after returning ptr may be equal to end
  static bool incrementOverWhitespace(const char*& ptr, const char* end) {
    unused(end); // we don't check multiple bytes yet
    char ch = *ptr;
    // all whitespace chars are either less than ' ' or take up more than one UTF8 byte, so the first byte will be negative!
    // this does rely on char being signed
    if ((signed char) ch > (signed char) ' ') return false;
    if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') {
      ++ptr;
      return true;
    }  // use the shift-trick here?

    // TODO: 0xA0 (non-breaking space) and the other unicode space chars (or java space chars)
    return false;
  }

public:
  WhitespaceTokenizer() : Tokenizer() {
  }

  virtual bool incrementToken() override {
    // first reset the token state
    token.clear();

    // first eat whitespace
    do {
      if (start_ >= end_) return false;
    } while (incrementOverWhitespace(start_, end_));

    // first non-whitespace, guaranteed to have start_ < end_
    const char* mystart = start_;

    // Going to the next character may place us on the second octet of a UTF8 sequence.
    // That's OK as long as our incrementOverWhitespace routine can handle that.
    // It's also guaranteed that this first character is not whitespace, so we don't need
    // to correctly match higher code points for whitespace.
    start_++;

    for (;;) {
      const char* myend = start_;
      if (start_ >= end_ || incrementOverWhitespace(start_, end_)) {
        // If we hit whitespace, we will have already skipped over the first whitespace char for the next call
        token.text = std::string_view(mystart, (size_t) (myend - mystart));
        token.setOffset((int) (mystart - base_), (int) (myend - base_));
        return true;
      }
      start_++;
    }
  }

  // An inlineable push version for performance experimentation
  template<class Sink>
  inline static void process(const char *val, int len, Sink sink) {
    const char *end = val + len;

    for (;;) {
      // first eat whitespace
      do {
        if (val >= end) return;
      } while (incrementOverWhitespace(val, end));

      const char *start = val;

      // find end of current token
      while (val < end && !incrementOverWhitespace(val, end)) {
        val++;  // TODO: an incrementIsWhitespace that always increments over a UTF8 code point and returns if it was whitespace or not?
      }

      // TODO: should sink return a boolean to continue or break out?
      sink(start, (int) (val - start));
    }
  }
};


// KeywordTokenizer passes the entire input as a single token
class KeywordTokenizer : public Tokenizer {
public:
  KeywordTokenizer() : Tokenizer() {}

  virtual bool incrementToken() override {
    // setValue repositions start_ at the value, so start_ >= end_ both signals
    // an empty value and marks the single token as already consumed.
    if (start_ >= end_) return false;
    token.clear();
    token.text = std::string_view(start_, (size_t) (end_ - start_));
    token.setOffset((int) (start_ - base_), (int) (end_ - base_));
    start_ = end_;  // consumed
    return true;
  }
};


// ASCII lowercase. Rewriting filters obey the read-only contract: rather than
// writing the source bytes in place, it copies the token into its own reusable
// buffer and repoints. Pure-lowercase tokens pass through with zero copy.
class LowercaseFilter : public TokenFilter {
  std::vector<char> buf; // reusable output buffer; the borrowed bytes live here after a rewrite
public:
  LowercaseFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;

    std::string_view in = token.text;
    // Find the first uppercase byte. If there is none, leave the token pointing
    // at the source bytes (no copy, no repoint).
    size_t i = 0;
    for (; i < in.size(); i++) {
      char c = in[i];
      if (c >= 'A' && c <= 'Z') break; // TODO: handle unicode!
    }
    if (i == in.size()) return true;

    // Copy into our own buffer and fold the rest. We never write `in`.
    buf.assign(in.begin(), in.end());
    for (; i < buf.size(); i++) {
      char c = buf[i];
      if (c >= 'A' && c <= 'Z') buf[i] = (char) (c + ('a' - 'A'));
    }
    token.text = std::string_view(buf.data(), buf.size());
    return true;
  }

  void normalizeTerm(std::string& term) override {
    TokenFilter::normalizeTerm(term);
    for (char& c : term) {
      if (c >= 'A' && c <= 'Z') c = (char) (c + ('a' - 'A'));
    }
  }
};

// UAX#29 word-boundary tokenizer and NFKC_CF (toNFKC_Casefold) fold filter, built
// on uni-algo. Defined in Analyzer.cpp so uni-algo's heavy Unicode headers stay
// out of this widely-included header. The unicode_word tokenizer is stateful (it
// carries a segmentation cursor and rebuilds it in reset()), so a chain using it
// must be constructed with stateful=true.
std::unique_ptr<Tokenizer> makeUnicodeWordTokenizer();
std::unique_ptr<TokenStream> makeNfkcCasefoldFilter(std::unique_ptr<TokenStream> source);
// Fused unicode_word + nfkc_cf: UAX#29 segmentation with per-token NFKC_CF in one
// stage. createAnalyzer swaps this in when it sees that canonical pair, so the
// common default avoids the extra filter hop. Equivalent output (see AnalysisTest).
std::unique_ptr<Tokenizer> makeStandardTokenizer();
// Accent/diacritic folding (cafe == cafe-with-accent). Lossy and language-
// dependent, so it is a separate opt-in filter applied after nfkc_cf.
std::unique_ptr<TokenStream> makeAccentFoldFilter(std::unique_ptr<TokenStream> source);

class TokenStreamFactory {


};

class TokenizerFactory : public TokenStreamFactory {

};

class TokenFilterFactory : public TokenStreamFactory {

};


// per-field version of Lucene's Analyzer
class FieldAnalyzer {
  // TODO: how do we go to something that is not thread-safe?

};

class TokenChain {
public:
  Tokenizer &head;                   // owned via `tail` (the head is the bottom of the chain)
  std::unique_ptr<TokenStream> tail; // the top of the chain; pull from here
  bool stateful;                     // true iff any stage buffers state across tokens/values

  TokenChain(Tokenizer &head, std::unique_ptr<TokenStream> tail, bool stateful = false)
          : head(head), tail(std::move(tail)), stateful(stateful) {}

  // Begin analysis of a fresh value (the value itself is supplied via
  // head.setValue). For the common stateless chain this is a single predictable
  // branch with no virtual dispatch; only a chain that contains a stateful
  // filter pays the source-ward reset() walk.
  void reset() {
    if (stateful) tail->reset();
  }

  // Normalize a single term the way this chain folds token text, without
  // segmentation - the multiterm entry point (see TokenStream::normalizeTerm).
  void normalizeTerm(std::string& term) { tail->normalizeTerm(term); }
};


// TODO... does index and query time have different analyzers, or should it be getIndexingChain/getQueryingChain?
// Analyzer class is thread safe
class Analyzer {
  // Lucene Analyzer always has to look up by fieldName... we should be able to avoid this
  TokenChain *createChain() {
    return nullptr;
  }
};


} // end namespace