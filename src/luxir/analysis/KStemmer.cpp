/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
This file was partially derived from the
original CIIR University of Massachusetts Amherst version of KStemmer.java (license for
the original shown below)
 */

/*
Copyright (c) 2003,
Center for Intelligent Information Retrieval,
University of Massachusetts, Amherst.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. The names "Center for Intelligent Information Retrieval" and
"University of Massachusetts" must not be used to endorse or promote products
derived from this software without prior written permission. To obtain
permission, contact info@ciir.cs.umass.edu.

THIS SOFTWARE IS PROVIDED BY UNIVERSITY OF MASSACHUSETTS AND OTHER CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
*/

// Copyright 2008, Luicid Imagination, Inc.
// Ported from Apache Lucene KStemmer.java; suffix rules and dictionaries are unchanged.

#include "luxir/analysis/KStemmer.h"
#include "luxir/analysis/KStemData.h"

#include <boost/unordered/unordered_flat_set.hpp>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace luxir {

namespace {

// An unchanged ending cannot start a rule chain. Include every rule's final
// three letters and every direct mapping's ending before ruling out a change.
constexpr auto makePossibleSuffixes() {
  std::array<uint32_t, 26 * 26> suffixes{};
  auto add = [&](std::string_view ending) {
    unsigned last = ending.back() - 'a';
    for (unsigned previous = 0; previous < 26; ++previous) {
      if (ending.size() >= 2 && previous != (unsigned) (ending[ending.size() - 2] - 'a')) continue;
      suffixes[last * 26 + previous] |= ending.size() < 3 ? (1u << 26) - 1
          : 1u << (ending[ending.size() - 3] - 'a');
    }
  };
  for (std::string_view ending : {"s", "ed", "ing", "ity", "ion", "er", "or", "ly", "al",
                                  "ive", "ize", "ment", "ble", "ism", "ic", "ncy", "nce"}) {
    add(ending);
  }
  for (size_t i = 0; i + 2 < sizeof(kstem::mappedEndings); i += 3) {
    add(std::string_view(kstem::mappedEndings + i, 3));
  }
  return suffixes;
}

// Six bits of key length, two bits of root kind, then a 24-bit metadata offset.
// Zero is reserved for no match.
constexpr uint32_t KeyLengthMask = 63;
constexpr uint32_t ExceptionRoot = 64;
constexpr uint32_t ReplacementRoot = 128;

const char* entryMetadata(uint32_t entry) {
  return kstem::data + (entry >> 8);
}

std::string_view entryKey(uint32_t entry) {
  unsigned keyLength = entry & KeyLengthMask;
  return std::string_view(entryMetadata(entry) - keyLength, keyLength);
}

bool isException(uint32_t entry) {
  return entry & ExceptionRoot;
}

std::string_view entryRoot(uint32_t entry) {
  if (!(entry & (ExceptionRoot | ReplacementRoot))) return {};
  if (isException(entry)) return entryKey(entry);
  const char* metadata = entryMetadata(entry);
  int rootLength = (unsigned char) *metadata;
  return std::string_view(metadata + 1, rootLength);
}

struct EntryHash {
  using is_transparent = void;
  using is_avalanching = boost::hash_is_avalanching<boost::hash<std::string_view>>;
  size_t operator()(std::string_view term) const noexcept {
    return boost::hash<std::string_view>{}(term);
  }
  size_t operator()(uint32_t entry) const noexcept { return (*this)(entryKey(entry)); }
};

struct EntryEqual {
  using is_transparent = void;
  bool operator()(uint32_t a, uint32_t b) const noexcept {
    return entryKey(a) == entryKey(b);
  }
  bool operator()(uint32_t a, std::string_view b) const noexcept { return entryKey(a) == b; }
  bool operator()(std::string_view a, uint32_t b) const noexcept { return a == entryKey(b); }
};

// Short keys fit beside their index entry. Include length in equality so
// embedded NULs cannot alias a shorter key. No dictionary bytes are read.
constexpr uint64_t ShortKeyMask = (1ull << 38) - 1;

uint64_t shortKey(std::string_view term) {
  assert(term.size() <= 4);
  uint32_t bytes = 0;
  if (term.size() == 4) {
    std::memcpy(&bytes, term.data(), 4);
  } else if (term.size() >= 2) {
    uint16_t first;
    std::memcpy(&first, term.data(), 2);
    bytes = first;
    if (term.size() == 3) bytes |= (uint32_t) (unsigned char) term[2] << 16;
  } else if (!term.empty()) {
    bytes = (unsigned char) term[0];
  }
  return bytes | ((uint64_t) term.size() << 32);
}

struct ShortHash {
  size_t operator()(uint64_t entry) const noexcept { return entry & ShortKeyMask; }
};

struct ShortEqual {
  bool operator()(uint64_t a, uint64_t b) const noexcept {
    return (a & ShortKeyMask) == (b & ShortKeyMask);
  }
};

} // namespace

constinit const std::array<uint32_t, 26 * 26> KStemmer::possibleSuffixes = makePossibleSuffixes();

struct KStemmer::Dictionary {
  // Long keys use four-byte indices into static data. Short keys also store
  // their bytes, so ordinary short-word hits never need to access that data.
  boost::unordered_flat_set<uint32_t, EntryHash, EntryEqual> entries;
  boost::unordered_flat_set<uint64_t, ShortHash, ShortEqual> shortEntries;
};

const KStemmer::Dictionary& KStemmer::getDictionary() {
  static const Dictionary dictionary = [] {
    static_assert(sizeof(kstem::data) <= (1u << 24));
    Dictionary d;
    d.entries.reserve(kstem::entryCount - kstem::shortEntryCount);
    d.shortEntries.reserve(kstem::shortEntryCount);
    const char* end = kstem::data + sizeof(kstem::data) - 1; // exclude the array's trailing NUL
    for (const char* key = kstem::data; key < end;) {
      // Metadata is never a lowercase ASCII letter, so the key length is only
      // needed in the index. Scan it once while constructing the dictionary.
      const char* metadata = key;
      while (metadata < end && *metadata >= 'a' && *metadata <= 'z') ++metadata;
      assert(metadata < end && metadata > key && metadata - key < 64);
      int rootLength = (unsigned char) *metadata;
      uint32_t kind = rootLength == 255 ? ExceptionRoot : rootLength == 0 ? 0 : ReplacementRoot;
      uint32_t entry = ((uint32_t) (metadata - kstem::data) << 8) | kind | (uint32_t) (metadata - key);
      bool inserted = metadata - key <= 4
          ? d.shortEntries.insert(((uint64_t) entry << 32) | shortKey(entryKey(entry))).second
          : d.entries.insert(entry).second;
      if (!inserted) {
        throw std::logic_error("Duplicate KStem dictionary entry: " + std::string(entryKey(entry)));
      }
      key = metadata + 1 + (rootLength == 255 ? 0 : rootLength);
      assert(key <= end);
    }
    assert(d.entries.size() + d.shortEntries.size() == kstem::entryCount);
    return d;
  }();
  return dictionary;
}

KStemmer::KStemmer() : dictionary(getDictionary()) {}

inline uint32_t KStemmer::find(std::string_view term) const {
  if (term.size() <= 4) {
    auto it = dictionary.shortEntries.find(shortKey(term));
    return it == dictionary.shortEntries.end() ? 0 : (uint32_t) (*it >> 32);
  }
  auto it = dictionary.entries.find(term);
  return it == dictionary.entries.end() ? 0 : *it;
}

inline void KStemmer::append(char ch) {
  assert(length >= 0 && length < (int) sizeof(word));
  word[length++] = ch;
}

inline void KStemmer::append(std::string_view suffix) {
  assert(length >= 0 && length + suffix.size() <= sizeof(word));
  std::memcpy(word + length, suffix.data(), suffix.size());
  length += (int) suffix.size();
}

inline bool KStemmer::isCons(int index) {
  char ch = word[index];
  if (ch == 'a' || ch == 'e' || ch == 'i' || ch == 'o' || ch == 'u') return false;
  if (ch != 'y' || index == 0) return true;
  return !isCons(index - 1);
}

inline bool KStemmer::endsIn(std::string_view s) {
  if ((int) s.size() > k) return false;

  int r = length - (int) s.size(); /* length of word before this suffix */
  j = k;
  for (int r1 = r, i = 0; i < (int) s.size(); i++, r1++) {
    if (s[i] != word[r1]) return false;
  }
  j = r - 1; /* index of the character BEFORE the posfix */
  return true;
}

inline bool KStemmer::endsIn(char a, char b) {
  if (2 > k) return false;
  // check left to right since the endings have often already matched
  if (word[k - 1] == a && word[k] == b) {
    j = k - 2;
    return true;
  }
  return false;
}

inline bool KStemmer::endsIn(char a, char b, char c) {
  if (3 > k) return false;
  if (word[k - 2] == a && word[k - 1] == b && word[k] == c) {
    j = k - 3;
    return true;
  }
  return false;
}

inline bool KStemmer::endsIn(char a, char b, char c, char d) {
  if (4 > k) return false;
  if (word[k - 3] == a
      && word[k - 2] == b
      && word[k - 1] == c
      && word[k] == d) {
    j = k - 4;
    return true;
  }
  return false;
}

inline uint32_t KStemmer::wordInDict() {
  if (matchedEntry != 0) return matchedEntry;
  uint32_t e = find(std::string_view(word, length));
  if (e != 0 && !isException(e)) {
    matchedEntry = e; // only cache if it's not an exception.
  }
  return e;
}

/* Convert plurals to singular form, and '-ies' to 'y' */
inline void KStemmer::plural() {
  if (word[k] == 's') {
    if (endsIn('i', 'e', 's')) {
      length = j + 3;
      k--;
      if (lookup()) /* ensure calories -> calorie */ return;
      k++;
      append('s');
      setSuffix("y");
      lookup();
    } else if (endsIn('e', 's')) {
      /* try just removing the "s" */
      length = j + 2;
      k--;

      /*
       * note: don't check for exceptions here. So, `aides' -> `aide', but
       * `aided' -> `aid'. The exception for double s is used to prevent
       * crosses -> crosse. This is actually correct if crosses is a plural
       * noun (a type of racket used in lacrosse), but the verb is much more
       * common
       */

      bool tryE = j > 0 && !((word[j] == 's') && (word[j - 1] == 's'));
      if (tryE && lookup()) return;

      /* try removing the "es" */

      length = j + 1;
      k--;
      if (lookup()) return;

      /* the default is to retain the "e" */
      append('e');
      k++;

      if (!tryE) lookup(); // if we didn't try the "e" ending before
      return;
    } else {
      if (length > 3 && word[k - 1] != 's' && !endsIn('o', 'u', 's')) {
        /* unless the word ends in "ous" or a double "s", remove the final "s" */

        length = k;
        k--;
        lookup();
      }
    }
  }
}

/* replace old suffix with s */
inline void KStemmer::setSuffix(std::string_view s) {
  length = j + 1;
  append(s);
  k = j + (int) s.size();
}

/* Returns true if the word is found in the dictionary */
// almost all uses of lookup() return immediately and are
// followed by another lookup in the dict. Store the match
// to avoid this double lookup.

inline bool KStemmer::lookup() {

  matchedEntry = find(std::string_view(word, length));
  return matchedEntry != 0;
}

/* convert past tense (-ed) to present, and `-ied' to `y' */
inline void KStemmer::pastTense() {
  /*
   * Handle words less than 5 letters with a direct mapping This prevents
   * (fled -> fl).
   */
  if (length <= 4) return;

  if (endsIn('i', 'e', 'd')) {
    length = j + 3;
    k--;
    if (lookup()) /* we almost always want to convert -ied to -y, but */
      return; /* this isn't true for short words (died->die) */
    k++; /* I don't know any long words that this applies to, */
    append('d'); /* but just in case... */
    setSuffix("y");
    lookup();
    return;
  }

  /* the vowelInStem() is necessary so we don't stem acronyms */
  if (endsIn('e', 'd') && vowelInStem()) {
    /* see if the root ends in `e' */
    length = j + 2;
    k = j + 1;

    uint32_t entry = wordInDict();
    if (entry != 0 && !isException(entry)) return;

    /* try removing the "ed" */
    length = j + 1;
    k = j;
    if (lookup()) return;

    /*
     * try removing a doubled consonant. if the root isn't found in the
     * dictionary, the default is to leave it doubled. This will correctly
     * capture `backfilled' -> `backfill' instead of `backfill' ->
     * `backfille', and seems correct most of the time
     */

    if (doubleC(k)) {
      length = k;
      k--;
      if (lookup()) return;
      append(word[k]);
      k++;
      lookup();
      return;
    }

    /* if we have a `un-' prefix, then leave the word alone */
    /* (this will sometimes screw up with `under-', but we */
    /* will take care of that later) */

    if ((word[0] == 'u') && (word[1] == 'n')) {
      append('e');
      append('d');
      k = k + 2;
      // nolookup()
      return;
    }

    /*
     * it wasn't found by just removing the `d' or the `ed', so prefer to end
     * with an `e' (e.g., `microcoded' -> `microcode').
     */

    length = j + 1;
    append('e');
    k = j + 1;
    // nolookup() - we already tried the "e" ending
    return;
  }
}

/* return TRUE if word ends with a double consonant */
inline bool KStemmer::doubleC(int i) {
  if (i < 1) return false;

  if (word[i] != word[i - 1]) return false;
  return (isCons(i));
}

inline bool KStemmer::vowelInStem() {
  for (int i = 0; i < (j + 1); i++) {
    if (!isCons(i)) return true;
  }
  return false;
}

/* handle `-ing' endings */
inline void KStemmer::aspect() {
  /*
   * handle short words (aging -> age) via a direct mapping. This prevents
   * (thing -> the) in the version of this routine that ignores inflectional
   * variants that are mentioned in the dictionary (when the root is also
   * present)
   */

  if (length <= 5) return;

  /* the vowelinstem() is necessary so we don't stem acronyms */
  if (endsIn('i', 'n', 'g') && vowelInStem()) {

    /* try adding an `e' to the stem and check against the dictionary */
    word[j + 1] = 'e';
    length = j + 2;
    k = j + 1;

    uint32_t entry = wordInDict();
    if (entry != 0) {
      if (!isException(entry)) /* if it's in the dictionary and not an exception */ return;
    }

    /* adding on the `e' didn't work, so remove it */
    length = k;
    k--; /* note that `ing' has also been removed */

    if (lookup()) return;

    /* if I can remove a doubled consonant and get a word, then do so */
    if (doubleC(k)) {
      k--;
      length = k + 1;
      if (lookup()) return;
      append(word[k]); /* restore the doubled consonant */

      /* the default is to leave the consonant doubled */
      /* (e.g.,`fingerspelling' -> `fingerspell'). Unfortunately */
      /* `bookselling' -> `booksell' and `mislabelling' -> `mislabell'). */
      /* Without making the algorithm significantly more complicated, this */
      /* is the best I can do */
      k++;
      lookup();
      return;
    }

    /*
     * the word wasn't in the dictionary after removing the stem, and then
     * checking with and without a final `e'. The default is to add an `e'
     * unless the word ends in two consonants, so `microcoding' ->
     * `microcode'. The two consonants restriction wouldn't normally be
     * necessary, but is needed because we don't try to deal with prefixes and
     * compounds, and most of the time it is correct (e.g., footstamping ->
     * footstamp, not footstampe; however, decoupled -> decoupl). We can
     * prevent almost all of the incorrect stems if we try to do some prefix
     * analysis first
     */

    if ((j > 0) && isCons(j) && isCons(j - 1)) {
      k = j;
      length = k + 1;
      // nolookup() because we already did according to the comment
      return;
    }

    length = j + 1;
    append('e');
    k = j + 1;
    // nolookup(); we already tried an 'e' ending
    return;
  }
}

/*
 * this routine deals with -ity endings. It accepts -ability, -ibility, and
 * -ality, even without checking the dictionary because they are so
 * productive. The first two are mapped to -ble, and the -ity is remove for
 * the latter
 */
inline void KStemmer::ityEndings() {
  int old_k = k;

  if (endsIn('i', 't', 'y')) {
    length = j + 1; /* try just removing -ity */
    k = j;
    if (lookup()) return;
    append('e'); /* try removing -ity and adding -e */
    k = j + 1;
    if (lookup()) return;
    word[j + 1] = 'i';
    append("ty");
    k = old_k;
    /*
     * the -ability and -ibility endings are highly productive, so just accept
     * them
     */
    if ((j > 0) && (word[j - 1] == 'i') && (word[j] == 'l')) {
      length = j - 1;
      append("le"); /* convert to -ble */
      k = j;
      lookup();
      return;
    }

    /* ditto for -ivity */
    if ((j > 0) && (word[j - 1] == 'i') && (word[j] == 'v')) {
      length = j + 1;
      append('e'); /* convert to -ive */
      k = j + 1;
      lookup();
      return;
    }
    /* ditto for -ality */
    if ((j > 0) && (word[j - 1] == 'a') && (word[j] == 'l')) {
      length = j + 1;
      k = j;
      lookup();
      return;
    }

    /*
     * if the root isn't in the dictionary, and the variant *is* there, then
     * use the variant. This allows `immunity'->`immune', but prevents
     * `capacity'->`capac'. If neither the variant nor the root form are in
     * the dictionary, then remove the ending as a default
     */

    if (lookup()) return;

    /* the default is to remove -ity altogether */
    length = j + 1;
    k = j;
    // nolookup(), we already did it.
    return;
  }
}

/* handle -ence and -ance */
inline void KStemmer::nceEndings() {
  int old_k = k;
  char word_char;

  if (endsIn('n', 'c', 'e')) {
    word_char = word[j];
    if (!((word_char == 'e') || (word_char == 'a'))) return;
    length = j;
    append('e'); /* try converting -e/ance to -e (adherance/adhere) */
    k = j;
    if (lookup()) return;
    length = j; /*
                        * try removing -e/ance altogether
                        * (disappearance/disappear)
                        */
    k = j - 1;
    if (lookup()) return;
    append(word_char); /* restore the original ending */
    append("nce");
    k = old_k;
    // nolookup() because we restored the original ending
  }
  return;
}

/* handle -ness */
inline void KStemmer::nessEndings() {
  if (endsIn('n', 'e', 's', 's')) {
    /*
     * this is a very productive endings, so
     * just accept it
     */
    length = j + 1;
    k = j;
    if (word[j] == 'i') word[j] = 'y';
    lookup();
  }
  return;
}

/* handle -ism */
inline void KStemmer::ismEndings() {
  if (endsIn('i', 's', 'm')) {
    /*
     * this is a very productive ending, so just
     * accept it
     */
    length = j + 1;
    k = j;
    lookup();
  }
  return;
}

/* this routine deals with -ment endings. */
inline void KStemmer::mentEndings() {
  int old_k = k;

  if (endsIn('m', 'e', 'n', 't')) {
    length = j + 1;
    k = j;
    if (lookup()) return;
    append("ment");
    k = old_k;
    // nolookup
  }
  return;
}

/* this routine deals with -ize endings. */
inline void KStemmer::izeEndings() {
  int old_k = k;

  if (endsIn('i', 'z', 'e')) {
    length = j + 1; /* try removing -ize entirely */
    k = j;
    if (lookup()) return;
    append('i');

    if (doubleC(j)) {
      /* allow for a doubled consonant */
      length = j;
      k = j - 1;
      if (lookup()) return;
      append(word[j - 1]);
    }

    length = j + 1;
    append('e'); /* try removing -ize and adding -e */
    k = j + 1;
    if (lookup()) return;
    length = j + 1;
    append("ize");
    k = old_k;
    // nolookup()
  }
  return;
}

/* handle -ency and -ancy */
inline void KStemmer::ncyEndings() {
  if (endsIn('n', 'c', 'y')) {
    if (!((word[j] == 'e') || (word[j] == 'a'))) return;
    word[j + 2] = 't'; /* try converting -ncy to -nt */
    length = j + 3;
    k = j + 2;

    if (lookup()) return;

    word[j + 2] = 'c'; /* the default is to convert it to -nce */
    append('e');
    k = j + 3;
    lookup();
  }
  return;
}

/* handle -able and -ible */
inline void KStemmer::bleEndings() {
  int old_k = k;
  char word_char;

  if (endsIn('b', 'l', 'e')) {
    if (!((word[j] == 'a') || (word[j] == 'i'))) return;
    word_char = word[j];
    length = j; /* try just removing the ending */
    k = j - 1;
    if (lookup()) return;
    if (doubleC(k)) {
      /* allow for a doubled consonant */
      length = k;
      k--;
      if (lookup()) return;
      k++;
      append(word[k - 1]);
    }
    length = j;
    append('e'); /* try removing -a/ible and adding -e */
    k = j;
    if (lookup()) return;
    length = j;
    append("ate"); /* try removing -able and adding -ate */
    /* (e.g., compensable/compensate) */
    k = j + 2;
    if (lookup()) return;
    length = j;
    append(word_char); /* restore the original values */
    append("ble");
    k = old_k;
    // nolookup()
  }
  return;
}

/*
 * handle -ic endings. This is fairly straightforward, but this is also the
 * only place we try *expanding* an ending, -ic -> -ical. This is to handle
 * cases like `canonic' -> `canonical'
 */
inline void KStemmer::icEndings() {
  if (endsIn('i', 'c')) {
    length = j + 3;
    append("al"); /* try converting -ic to -ical */
    k = j + 4;
    if (lookup()) return;

    word[j + 1] = 'y'; /* try converting -ic to -y */
    length = j + 2;
    k = j + 1;
    if (lookup()) return;

    word[j + 1] = 'e'; /* try converting -ic to -e */
    if (lookup()) return;

    length = j + 1; /* try removing -ic altogether */
    k = j;
    if (lookup()) return;
    append("ic"); /* restore the original ending */
    k = j + 2;
    // nolookup()
  }
  return;
}

static constexpr std::string_view ization = "ization";
static constexpr std::string_view ition = "ition";
static constexpr std::string_view ation = "ation";
static constexpr std::string_view ication = "ication";

/* handle some derivational endings */
/*
 * this routine deals with -ion, -ition, -ation, -ization, and -ication. The
 * -ization ending is always converted to -ize
 */
inline void KStemmer::ionEndings() {
  int old_k = k;
  if (!endsIn('i', 'o', 'n')) {
    return;
  }

  if (endsIn(ization)) {
    /*
     * the -ize ending is very productive, so simply
     * accept it as the root
     */
    length = j + 3;
    append('e');
    k = j + 3;
    lookup();
    return;
  }

  if (endsIn(ition)) {
    length = j + 1;
    append('e');
    k = j + 1;
    if (lookup()) {
      // remove -ition and add `e', and check against the
      return; /* (e.g., definition->define, opposition->oppose) */
    }

    /* restore original values */
    length = j + 1;
    append("ition");
    k = old_k;
    // nolookup()
  } else if (endsIn(ation)) {
    length = j + 3;
    append('e');
    k = j + 3;
    if (lookup()) {
      /* remove -ion and add `e', and check against the dictionary */
      return; /* (elimination -> eliminate) */
    }

    length = j + 1;
    append('e');
    // remove -ation and add `e', and check against the dictionary
    k = j + 1;
    if (lookup()) return;

    length = j + 1;
    // just remove -ation (resignation->resign) and check dictionary
    k = j;
    if (lookup()) return;

    /* restore original values */
    length = j + 1;
    append("ation");
    k = old_k;
    // nolookup()

  }

  /*
   * test -ication after -ation is attempted (e.g., `complication->complicate'
   * rather than `complication->comply')
   */

  if (endsIn(ication)) {
    length = j + 1;
    append('y');
    k = j + 1;
    if (lookup()) {
      // remove -ication and add `y', and check against the dictionary
      return; /* (e.g., amplification -> amplify) */
    }

    /* restore original values */
    length = j + 1;
    append("ication");
    k = old_k;
    // nolookup()
  }

  // The -ion ending was checked above; restore j after the other suffix probes.
  j = k - 3;

  length = j + 1;
  append('e');
  k = j + 1;
  if (lookup()) /* remove -ion and add `e', and check against the dictionary */ return;

  length = j + 1;
  k = j;
  if (lookup()) /* remove -ion, and if it's found, treat that as the root */ return;

  /* restore original values */
  length = j + 1;
  append("ion");
  k = old_k;
  // nolookup()

  // nolookup(); all of the other paths restored original values
  return;
}

/*
 * this routine deals with -er, -or, -ier, and -eer. The -izer ending is
 * always converted to -ize
 */
inline void KStemmer::erAndOrEndings() {
  int old_k = k;

  if (word[k] != 'r') return; // YCS

  char word_char; /* so we can remember if it was -er or -or */

  if (endsIn('i', 'z', 'e', 'r')) {
    /*
     * -ize is very productive, so accept it
     * as the root
     */
    length = j + 4;
    k = j + 3;
    lookup();
    return;
  }

  if (endsIn('e', 'r') || endsIn('o', 'r')) {
    word_char = word[j + 1];
    if (doubleC(j)) {
      length = j;
      k = j - 1;
      if (lookup()) return;
      append(word[j - 1]); /* restore the doubled consonant */
    }

    if (word[j] == 'i') {
      /* do we have a -ier ending? */
      word[j] = 'y';
      length = j + 1;
      k = j;
      if (lookup()) /* yes, so check against the dictionary */ return;
      word[j] = 'i'; /* restore the endings */
      append('e');
    }

    if (word[j] == 'e') {
      /* handle -eer */
      length = j;
      k = j - 1;
      if (lookup()) return;
      append('e');
    }

    length = j + 2; /* remove the -r ending */
    k = j + 1;
    if (lookup()) return;
    length = j + 1; /* try removing -er/-or */
    k = j;
    if (lookup()) return;
    append('e'); /* try removing -or and adding -e */
    k = j + 1;
    if (lookup()) return;
    length = j + 1;
    append(word_char);
    append('r'); /* restore the word to the way it was */
    k = old_k;
    // nolookup()
  }
}

/*
 * this routine deals with -ly endings. The -ally ending is always converted
 * to -al Sometimes this will temporarily leave us with a non-word (e.g.,
 * heuristically maps to heuristical), but then the -al is removed in the next
 * step.
 */
inline void KStemmer::lyEndings() {
  int old_k = k;

  if (endsIn('l', 'y')) {

    word[j + 2] = 'e'; /* try converting -ly to -le */

    if (lookup()) return;
    word[j + 2] = 'y';

    length = j + 1; /* try just removing the -ly */
    k = j;

    if (lookup()) return;

    // Always convert -ally to -al.
    if ((j > 0) && (word[j - 1] == 'a') && (word[j] == 'l')) return;
    append("ly");
    k = old_k;

    if ((j > 0) && (word[j - 1] == 'a') && (word[j] == 'b')) {
      // Always convert -ably to -able.
      word[j + 2] = 'e';
      k = j + 2;
      return;
    }

    if (word[j] == 'i') {
      /* e.g., militarily -> military */
      length = j;
      append('y');
      k = j;
      if (lookup()) return;
      length = j;
      append("ily");
      k = old_k;
    }

    length = j + 1; /* the default is to remove -ly */

    k = j;
    // nolookup()... we already tried removing the "ly" variant
  }
  return;
}

/*
 * this routine deals with -al endings. Some of the endings from the previous
 * routine are finished up here.
 */
inline void KStemmer::alEndings() {
  int old_k = k;

  if (length < 4) return;
  if (endsIn('a', 'l')) {
    length = j + 1;
    k = j;
    if (lookup()) /* try just removing the -al */ return;

    if (doubleC(j)) {
      /* allow for a doubled consonant */
      length = j;
      k = j - 1;
      if (lookup()) return;
      append(word[j - 1]);
    }

    length = j + 1;
    append('e'); /* try removing the -al and adding -e */
    k = j + 1;
    if (lookup()) return;

    length = j + 1;
    append("um"); /* try converting -al to -um */
    /* (e.g., optimal - > optimum ) */
    k = j + 2;
    if (lookup()) return;

    length = j + 1;
    append("al"); /* restore the ending to the way it was */
    k = old_k;

    if ((j > 0) && (word[j - 1] == 'i') && (word[j] == 'c')) {
      length = j - 1; /* try removing -ical */
      k = j - 2;
      if (lookup()) return;

      length = j - 1;
      append('y'); /* try turning -ical to -y (e.g., bibliographical) */
      k = j - 1;
      if (lookup()) return;

      length = j - 1;
      append("ic"); /* the default is to convert -ical to -ic */
      k = j;
      // nolookup() ... converting ical to ic means removing "al" which we
      // already tried
      lookup();
      return;
    }

    if (word[j] == 'i') {
      /* sometimes -ial endings should be removed */
      length = j; /* (sometimes it gets turned into -y, but we */
      k = j - 1; /* aren't dealing with that case for now) */
      if (lookup()) return;
      append("ial");
      k = old_k;
      lookup();
    }
  }
  return;
}

/*
 * this routine deals with -ive endings. It normalizes some of the -ative
 * endings directly, and also maps some -ive endings to -ion.
 */
inline void KStemmer::iveEndings() {
  int old_k = k;

  if (endsIn('i', 'v', 'e')) {
    length = j + 1; /* try removing -ive entirely */
    k = j;
    if (lookup()) return;

    append('e'); /* try removing -ive and adding -e */
    k = j + 1;
    if (lookup()) return;
    length = j + 1;
    append("ive");
    if ((j > 0) && (word[j - 1] == 'a') && (word[j] == 't')) {
      word[j - 1] = 'e'; /* try removing -ative and adding -e */
      length = j; /* (e.g., determinative -> determine) */
      k = j - 1;
      if (lookup()) return;
      length = j - 1; /* try just removing -ative */
      if (lookup()) return;

      append("ative");
      k = old_k;
    }

    /* try mapping -ive to -ion (e.g., injunctive/injunction) */
    word[j + 2] = 'o';
    word[j + 3] = 'n';
    if (lookup()) return;

    word[j + 2] = 'v'; /* restore the original values */
    word[j + 3] = 'e';
    k = old_k;
    // nolookup()
  }
  return;
}

std::string_view KStemmer::stemCandidate(std::string_view term) {
  if (uint32_t entry = find(term)) {
    auto root = entryRoot(entry);
    return root.empty() ? term : root;
  }
  return stemUnknown(term);
}

std::string_view KStemmer::stemUnknown(std::string_view term) {
  for (char ch : term) {
    if (ch < 'a' || ch > 'z') return term;
  }
  length = (int) term.size();
  std::memcpy(word, term.data(), term.size());
  k = length - 1;
  matchedEntry = 0;

  // One pass; stop as soon as a dictionary entry is recognized. Rule order is
  // semantic, including the extra dictionary lookup after -al.
  do {
    plural();
    if (matchedEntry) break;
    pastTense();
    if (matchedEntry) break;
    aspect();
    if (matchedEntry) break;
    ityEndings();
    if (matchedEntry) break;
    nessEndings();
    if (matchedEntry) break;
    ionEndings();
    if (matchedEntry) break;
    erAndOrEndings();
    if (matchedEntry) break;
    lyEndings();
    if (matchedEntry) break;
    alEndings();
    if (matchedEntry) break;
    wordInDict();
    iveEndings();
    if (matchedEntry) break;
    izeEndings();
    if (matchedEntry) break;
    mentEndings();
    if (matchedEntry) break;
    bleEndings();
    if (matchedEntry) break;
    ismEndings();
    if (matchedEntry) break;
    icEndings();
    if (matchedEntry) break;
    ncyEndings();
    if (matchedEntry) break;
    nceEndings();
  } while (false);

  if (matchedEntry) {
    auto root = entryRoot(matchedEntry);
    if (!root.empty()) return root;
  }
  assert(length >= 0 && length <= (int) sizeof(word));
  std::string_view result(word, length);
  return result == term ? term : result;
}

} // namespace luxir
