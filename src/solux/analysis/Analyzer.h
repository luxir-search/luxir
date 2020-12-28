#pragma once

#include <string>
#include <assert.h>
#include <climits>
#include <memory>
#include "solux/util/solux_util.h"

//
// TODO: investigate using string view?
//
class Token {
private:
  // TODO: need move constructors.
  // If ptr doesn't point to buf, then we should make a copy into buf

  // ptr may or may not point to buf.
  std::string buf;
public:
  char* ptr;
  char* end;
  int positionIncrement; // position or increment?
  // optional way to get pointer to Indexer (or make that a thread local?)... may be used in other contexts

  void clear() {
    positionIncrement = 1;
  }

  int numBytes() {
    assert( end - ptr < INT_MAX );
    return (int)(end - ptr);
  }

};



// OPTION: we can either have the token ourselves, or have the caller pass it in
// If the caller passes it in, everyone in the chain would need to set it (for a short field?)
// If we return it, it's a virtual method call???
class TokenStream {
protected:
  Token& token;
public:
  TokenStream(Token& tok) : token(tok) {
  }

  virtual ~TokenStream() {}

  Token& getToken() { return token; }

  virtual bool incrementToken(bool first) = 0;
};

class Tokenizer : public TokenStream {
protected:
  char* start_;
  char* end_;
public:
  Tokenizer(Token &tok) : TokenStream(tok) { }

  /** The value this points to should exist for the duration of the analysis and may be changed in place! */
  void setMutableValue(char* val, int len) {
    assert(len >= 0); // do we mave a max size as well?
    start_ = val;
    end_ = val + len;
  }
};


class TokenFilter : public TokenStream {
  std::unique_ptr<TokenFilter> source_;
public:
  TokenFilter(Token &tok, std::unique_ptr<TokenFilter> source) : TokenStream(tok) , source_(std::move(source)) { }

  TokenFilter& source() { return *source_; }
};

// TODO: implement a test tokenizer like I did previously for my splitting filter in Lucene (and add payload support?)
//  foo bar/3 baz/0|my_payload

// TODO: keep track of offset!!!!!!!!!!!!!!!!! will need for highlighting!

class WhitespaceTokenizer : public Tokenizer {

    // increments over a single whitespace char.
    // assumes ptr is less than end
    // after returning ptr may be equal to end
    static bool incrementOverWhitespace(char *&ptr, const char *end) {
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
    WhitespaceTokenizer(Token &tok) : Tokenizer(tok) {}

    // TODO: somehow add a method that could be used via templates
    // or something that could be inlined (i.e. delegation via templates)

    virtual bool incrementToken(bool first) override {
        unused(first);
        // first reset the token state
        token.clear();

        // first eat whitespace
        do {
            if (start_ >= end_) return false;
        } while (incrementOverWhitespace(start_, end_));

        // first non-whitespace, guaranteed to have start_ < end_
        token.ptr = start_;

        // Going to the next character may place us on the second octet of a UTF8 sequence.
        // That's OK as long as our incrementOverWhitespace routine can handle that.
        // It's also guaranteed that this first character is not whitespace, so we don't need
        // to correctly match higher code points for whitespace.
        start_++;

        for (;;) {
            token.end = start_;
            if (start_ >= end_ || incrementOverWhitespace(start_, end_)) {
                // If we hit whitespace, we will have already skipped over the first whitespace char for the next call
                return true;
            }
            start_++;
        }
    }

    // An inlineable push version for performance experimentation
    template<class Sink>
    inline static void process(char *val, int len, Sink sink) {
        const char* end = val + len;

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
            sink(start, (int)(val - start));
        }
    }


};


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
  std::unique_ptr<Token> token; // TODO: just include directly?
  Tokenizer& head;
  std::unique_ptr<TokenStream> tail;

  TokenChain(std::unique_ptr<Token> token, Tokenizer &head, std::unique_ptr<TokenStream> tail)
          : token(std::move(token)), head(head), tail(std::move(tail))
  { }
};


// TODO... does index and query time have different analyzers, or should it be getIndexingChain/getQueryingChain?
// Analyzer class is thread safe
class Analyzer {
  // Lucene Analyzer always has to look up by fieldName... we should be able to avoid this
  TokenChain& createChain() {
    return *(TokenChain*)0;
  }
};





/***

  fieldInfo.getFieldType()

  // ask a field value to index itself?
  // what about multi-valued fields?
  fieldValue.index(Indexer& indexer)

// wait... we already look up something per-field in the indexer... SegmentField... that's where the reusable analyzer (if needed) should live!

 */


