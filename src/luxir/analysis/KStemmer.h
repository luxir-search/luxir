// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace luxir {

// Lucene's dictionary-based Krovetz English stemmer. Input must already be
// lowercase ASCII; other tokens and lengths outside [3, 49] pass through.
// Each instance owns scratch space and is not thread-safe. The dictionary is
// immutable and shared by all instances. No per-token allocations.
class KStemmer {
  struct Dictionary;
  const Dictionary& dictionary;
  static constexpr int MaxWordLen = 50;
  static const std::array<uint32_t, 26 * 26> possibleSuffixes;
  // Rules sometimes shorten and then restore the word by length alone. Bytes
  // past length must survive, unlike std::string::resize(). Allow expansions.
  char word[MaxWordLen + 10]{};
  int length = 0;
  int j = 0; // last character before the suffix
  int k = 0; // last character of the word
  uint32_t matchedEntry = 0; // key length (6), root kind (2), metadata offset (24)

public:
  KStemmer();

  // Borrows unchanged input, static dictionary storage, or this instance's
  // scratch buffer. The result is valid until the next stem() call.
  std::string_view stem(std::string_view term) {
    if (term.size() < 3 || term.size() >= MaxWordLen) return term;
    // A false result proves identity; candidates still go through the dictionary.
    unsigned last = (unsigned char) term.back() - 'a';
    unsigned previous = (unsigned char) term[term.size() - 2] - 'a';
    unsigned third = (unsigned char) term[term.size() - 3] - 'a';
    if (last >= 26 || previous >= 26 || third >= 26
        || !(possibleSuffixes[last * 26 + previous] & (1u << third))) return term;
    return stemCandidate(term);
  }

private:
  static const Dictionary& getDictionary();
  std::string_view stemCandidate(std::string_view term);
  std::string_view stemUnknown(std::string_view term);
  inline uint32_t find(std::string_view term) const;
  inline void append(char ch);
  inline void append(std::string_view suffix);
  inline bool isCons(int index);
  inline bool endsIn(std::string_view s);
  inline bool endsIn(char a, char b);
  inline bool endsIn(char a, char b, char c);
  inline bool endsIn(char a, char b, char c, char d);
  inline uint32_t wordInDict();
  inline void plural();
  inline void setSuffix(std::string_view s);
  inline bool lookup();
  inline void pastTense();
  inline bool doubleC(int i);
  inline bool vowelInStem();
  inline void aspect();
  inline void ityEndings();
  inline void nceEndings();
  inline void nessEndings();
  inline void ismEndings();
  inline void mentEndings();
  inline void izeEndings();
  inline void ncyEndings();
  inline void bleEndings();
  inline void icEndings();
  inline void ionEndings();
  inline void erAndOrEndings();
  inline void lyEndings();
  inline void alEndings();
  inline void iveEndings();
};

} // namespace luxir
