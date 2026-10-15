// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include "luxir/analysis/EnglishPossessive.h"

namespace luxir {

// Dictionary-based Krovetz English stemmer, ported from Apache Lucene.
// Input must already be lowercase ASCII. Plain stemming passes through other
// tokens and lengths outside [3, 49]. Optional possessive removal applies
// before those limits.
// Each instance owns scratch space and is not thread-safe. The dictionary is
// immutable and shared by all instances. No per-token allocations (the memo
// is allocated once per instance).
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

  // Memo of recent candidate stems, direct-mapped by a hash of the term. Text
  // is Zipfian, so most candidates repeat within a document and a hit skips
  // the dictionary probe and the rule chain. Results are a pure function of
  // the term, so a hit is exactly what stemCandidate would return. One entry
  // is half a cache line (4096 entries, 128KB per instance); longer terms
  // bypass the memo. Smaller memos thrash on a real vocabulary and cost more
  // in misses than they save (measured with BM_KStem on the book corpus).
  static constexpr int MemoBits = 12;
  static constexpr size_t MemoKeyMax = 15;
  struct MemoEntry {
    uint8_t keyLen = 0;   // 0 = empty (memoized terms are >= 3 bytes)
    uint8_t stemLen = 0;  // Unchanged: the stem is the term itself
    char key[MemoKeyMax];
    char stem[MemoKeyMax];
  };
  static_assert(sizeof(MemoEntry) == 32);
  static constexpr uint8_t Unchanged = 255;
  std::unique_ptr<MemoEntry[]> memo;  // allocated on the first candidate

public:
  KStemmer();

  // Borrows unchanged input, static dictionary storage, or this instance's
  // scratch buffer. The result is valid until the next stem() call.
  // Possessive=true is equivalent to stem<false>(removeEnglishPossessive(term)).
  // Select it at chain construction, leaving no runtime option on each token.
  template <bool Possessive = true>
  std::string_view stem(std::string_view term) {
    if constexpr (Possessive) term = removeEnglishPossessive(term);
    if (term.size() < 3 || term.size() >= MaxWordLen) return term;
    // A false result proves identity; candidates still go through the dictionary.
    unsigned last = (unsigned char) term.back() - 'a';
    unsigned previous = (unsigned char) term[term.size() - 2] - 'a';
    unsigned third = (unsigned char) term[term.size() - 3] - 'a';
    if (last >= 26 || previous >= 26 || third >= 26
        || !(possibleSuffixes[last * 26 + previous] & (1u << third))) return term;
    if (term.size() > MemoKeyMax) return stemCandidate(term);
    if (!memo) [[unlikely]] memo.reset(new MemoEntry[(size_t) 1 << MemoBits]);
    MemoEntry& e = memo[memoSlot(term)];
    if (e.keyLen == term.size() && std::memcmp(e.key, term.data(), term.size()) == 0) {
      // keep borrowing the input when stemming leaves it alone
      return e.stemLen == Unchanged ? term : std::string_view(e.stem, e.stemLen);
    }
    std::string_view stem = stemCandidate(term);
    if (stem.data() == term.data() && stem.size() == term.size()) {
      e.keyLen = (uint8_t) term.size();
      std::memcpy(e.key, term.data(), term.size());
      e.stemLen = Unchanged;
    } else if (stem.size() <= MemoKeyMax) {
      e.keyLen = (uint8_t) term.size();
      std::memcpy(e.key, term.data(), term.size());
      e.stemLen = (uint8_t) stem.size();
      std::memcpy(e.stem, stem.data(), stem.size());
    }
    return stem;
  }

private:
  // Terms here are 3..MemoKeyMax bytes: hash the first and last 8 (or 4, or
  // 2) bytes, overlapping for short terms, and the length.
  static size_t memoSlot(std::string_view term) {
    uint64_t a = 0, b = 0;
    size_t n = term.size();
    size_t w = n >= 8 ? 8 : n >= 4 ? 4 : 2;
    std::memcpy(&a, term.data(), w);
    std::memcpy(&b, term.data() + n - w, w);
    uint64_t h = (a ^ (b * 0x9E3779B97F4A7C15ull) ^ n) * 0xD6E8FEB86659FD93ull;
    return (size_t) (h >> (64 - MemoBits));
  }

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
