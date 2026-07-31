#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <vector>
#include <span>
#include <solux/util/screaming.h>
#include <solux/util/solux_util.h>

using namespace screaming;
namespace solux {

class DocSetBuilder;

class DocSet {
  friend DocSetBuilder;
protected:
  int32_t card_ = -1;
  virtual int32_t calcCard(){ return -1;};

public:
  enum Type {
    ARRAY = 0,
    BITSET = 1
  };
  const Type type;

  DocSet(Type type): type(type) {}

  /// returns the cardinality, which may involve calculating it first if it's not already known.
  int32_t card() {
    if (card_ == -1) {
      card_ = calcCard();
    }
    return card_;
  }

  /// returns either -1 for unknown, or the cached cardinality.
  int32_t cachedCard() const {
    return card_;
  }

  void setCard(int32_t card) {
    card_ = card;
  }

  virtual bool get(int32_t docid) const = 0; // returns true if the docid is in the set

  // Heap bytes owned by this DocSet, including capacity rather than logical
  // cardinality. Cache accounting uses this after immutable publication.
  virtual size_t ramBytesUsed() const = 0;

  static std::unique_ptr<DocSet> intersect(std::span<DocSet*> sets);

  /// Union two or more DocSets into a single new DocSet.  All BITSET inputs
  /// must share a size (caller's responsibility - typically all from the same
  /// segment's maxDoc).  Result is BITSET if any input is BITSET, otherwise
  /// ARRAY.
  static std::unique_ptr<DocSet> union_(std::span<DocSet*> sets);

  virtual ~DocSet() = default;
};

/// non-owning BitDocSet
/// Used for segment liveDocs.
class BitDocSet : public DocSet {
protected:
  FixedBitSet bits_; // non-owning bitset

  // Lazy popcount.  card() will call this once on first query and cache the
  // result in DocSet::card_; if the caller already knows the cardinality
  // (e.g. liveDocs), use the explicit-card ctor to skip the popcount entirely.
  int32_t calcCard() override {
    return bits_.card();
  }

public:
  BitDocSet(FixedBitSet bits) : DocSet(BITSET), bits_(bits) {
  }

  BitDocSet(FixedBitSet bits, int32_t card) : DocSet(BITSET), bits_(bits) {
    card_ = card;
  }

  bool get(int32_t docid) const override {
    return bits_.get(docid);
  }

  const FixedBitSet& bits() const {
    return bits_;
  };

  // temporary... remove this in the future.
  FixedBitSet& mutableBits() {
    return bits_;
  }

  size_t ramBytesUsed() const override {
    // The words are borrowed (liveDocs and other mapped views).
    return sizeof(BitDocSet);
  }
};


/// BitDocSet owning the DocSet memory.
class RAMBitDocSet : public BitDocSet {
public:
  RAMBitDocSet(int32_t size) : BitDocSet(FixedBitSet(FixedBitSet::allocate(size, false).release(), size)) {}

  RAMBitDocSet(RAMBitDocSet&& other) noexcept : BitDocSet(std::move(other.bits_)) {
    card_ = other.card_;
    other.bits_.words = nullptr;
  }

  RAMBitDocSet& operator=(RAMBitDocSet&& other) = delete;

  ~RAMBitDocSet() override {
    delete[] bits_.words; // free the allocated memory
  }

  size_t ramBytesUsed() const override {
    return sizeof(RAMBitDocSet)
        + FixedBitSet::sizeInWords(bits_.size()) * sizeof(uint64_t);
  }
};

/// Sorted array of docs.
class ArrDocSet : public DocSet {
protected:
  std::vector<int32_t> docs_;

  int32_t calcCard() override {
    return static_cast<int32_t>(docs_.size());
  }

public:
  ArrDocSet(std::vector<int32_t>&& docs) : DocSet(ARRAY), docs_(std::move(docs)) {
    if (docs_.data() == nullptr) {
      // Normalize empty storage so docs().data() is never null: consumers may
      // use a null pointer to mean "no set at all" (e.g. DocSetProbe), which
      // must stay distinct from an empty set that rejects every doc.
      docs_.reserve(1);
    }
    card_ = static_cast<int32_t>(docs_.size());
  }

  std::span<int32_t> docs() {
    return docs_;
  }

  std::span<const int32_t> docs() const {
    return docs_;
  }

  bool get(int32_t docid) const override {
    // do a binary search for the docid
    return std::binary_search(docs_.begin(), docs_.end(), docid);
  }

  size_t ramBytesUsed() const override {
    return sizeof(ArrDocSet) + docs_.capacity() * sizeof(int32_t);
  }

};

/// AND a window-local bitset (bit 0 == windowStart) with a segment-wide
/// FixedBitSet (bit 0 == doc 0). Words outside [windowStart, windowEnd) are
/// cleared, including the unused tail of a fixed-size scratch window.
inline void intersectBitSetWindow(std::span<uint64_t> windowWords,
                                  int32_t windowStart, int32_t windowEnd,
                                  const FixedBitSet& filter) {
  assert(windowStart >= 0);
  assert(windowEnd >= windowStart);
  assert(windowEnd <= filter.size());
  assert((int64_t) windowWords.size() * 64 >= windowEnd - windowStart);

  const int32_t filterWords =
      (int32_t) FixedBitSet::sizeInWords(filter.size());
  for (int32_t w = 0; w < (int32_t) windowWords.size(); w++) {
    int32_t firstDoc = windowStart + (w << 6);
    int32_t remaining = windowEnd - firstDoc;
    if (remaining <= 0) {
      windowWords[(size_t) w] = 0;
      continue;
    }

    int32_t sourceWord = firstDoc >> 6;
    int32_t shift = firstDoc & 63;
    uint64_t filterWord = filter.words[sourceWord] >> shift;
    if (shift != 0 && sourceWord + 1 < filterWords) {
      filterWord |= filter.words[sourceWord + 1] << (64 - shift);
    }
    if (remaining < 64) {
      filterWord &= (1ULL << remaining) - 1ULL;
    }
    windowWords[(size_t) w] &= filterWord;
  }
}

/// Galloping membership cursor over a sorted doc array. Probes gallop from
/// the previous landing, so an ascending scan costs O(log gap) per probe on
/// warm lines instead of a full binary search; a descending probe restarts
/// from the array base - correct, just slower. An empty (or default) probe
/// rejects every doc. The cursor is private to one scan: the underlying set
/// stays shared and immutable, so never share a probe across concurrent
/// consumers. Use directly when the set type is statically known; DocSetProbe
/// wraps one for the dynamic case.
class ArrDocSetProbe {
  const int32_t* base = nullptr;
  const int32_t* lo = nullptr;
  const int32_t* hi = nullptr;

public:
  ArrDocSetProbe() = default;

  explicit ArrDocSetProbe(const ArrDocSet& set) {
    reset(set);
  }

  explicit ArrDocSetProbe(std::span<const int32_t> docs) {
    reset(docs);
  }

  void reset(const ArrDocSet& set) {
    reset(set.docs());
  }

  void reset(std::span<const int32_t> docs) {
    base = lo = docs.data();
    hi = base + docs.size();
  }

  /// Bound to an array? ArrDocSet storage is never null, so binding one
  /// always answers true - including an empty set, which rejects every doc.
  bool bound() const {
    return base != nullptr;
  }

  bool get(int32_t doc) {
    // lo[-1] < doc means lower_bound(doc) >= lo, so the cursor still applies
    // even when this probe is below the previous one.
    const int32_t* start = (lo == base || lo[-1] < doc) ? lo : base;
    lo = screaming::gallopLowerBound(start, hi, doc);
    return lo != hi && *lo == doc;
  }

  // intersect() strategy: with k array docs overlapping the span's range and
  // n span docs, drive the array below k < n/8, gallop the array above
  // k > 32n, merge linearly between. Two O(1) lookahead probes classify the
  // block; no exact k is computed. The window is asymmetric because the
  // merge's linear array walk is prefetch-friendly: measured 2026-07-29
  // (n=128 spans, desktop) merge still beat the gallop arm at k/n=8
  // (8.3ns vs 10.0ns per span doc) and only tied at k/n=64; the gallop arm's
  // job is staying O(n log k) when k/n is extreme. Spans below 16 docs skip
  // classification and take the always-safe gallop arm.
  static constexpr int32_t INTERSECT_SPARSE_RATIO = 8;
  static constexpr int32_t INTERSECT_DENSE_RATIO = 32;
  static constexpr int32_t INTERSECT_MIN_CLASSIFY = 16;

  /// Invoke callback(doc, index) for every element of the strictly-ascending
  /// span `docs` that is a member of the array, in span order. Equivalent to
  /// (but faster than) calling get() per element: whole-block misses return
  /// after one gallop, and the walk strategy is chosen per block. Mark the
  /// callback SOLUX_INLINE at the call site.
  template <class F>
  void intersect(std::span<const int32_t> docs, F&& callback) {
    #ifndef NDEBUG
    for (size_t j = 1; j < docs.size(); j++) {
      assert(docs[j] > docs[j - 1]);
    }
    #endif
    const int32_t n = (int32_t) docs.size();
    if (n == 0 || base == hi) {
      return;
    }
    const int32_t first = docs.front();
    const int32_t last = docs.back();
    const int32_t* start = (lo == base || lo[-1] < first) ? lo : base;
    const int32_t* p = screaming::gallopLowerBound(start, hi, first);
    if (p == hi || *p > last) {
      lo = p;  // whole block misses
      return;
    }

    const int64_t avail = hi - p;
    if (n >= INTERSECT_MIN_CLASSIFY) {
      const int64_t denseProbe = (int64_t) n * INTERSECT_DENSE_RATIO;
      const int64_t sparseProbe = n / INTERSECT_SPARSE_RATIO;
      if (avail <= sparseProbe || p[sparseProbe] > last) {
        // k << n: drive the array, galloping a cursor over the span.
        const int32_t* s = docs.data();
        const int32_t* sEnd = s + n;
        while (p != hi && *p <= last) {
          int32_t v = *p;
          s = screaming::gallopLowerBound(s, sEnd, v);
          assert(s != sEnd);  // v <= last, so an element >= v exists
          if (*s == v) {
            callback(v, (int32_t) (s - docs.data()));
          }
          p++;
        }
        lo = p;
        return;
      }
      if (!(avail > denseProbe && p[denseProbe] <= last)) {
        // Comparable densities (n/8 <= k <= 32n): linear merge, cost <= 33n.
        int32_t i = 0;
        for (;;) {
          if (*p < docs[(size_t) i]) {
            if (++p == hi) break;
          } else if (*p > docs[(size_t) i]) {
            if (++i == n) break;
          } else {
            callback(*p, i);
            ++p;
            ++i;
            if (p == hi || i == n) break;
          }
        }
        lo = p;
        return;
      }
      // else k >> n: fall through to the gallop arm.
    }

    // Drive the span, galloping the array from the cursor: get()-equivalent
    // per element, never linear in k. The default for unclassified spans.
    for (int32_t i = 0; i < n; i++) {
      int32_t d = docs[(size_t) i];
      if (*p < d) {
        p = screaming::gallopLowerBound(p, hi, d);
        if (p == hi) break;
      }
      if (*p == d) {
        callback(d, i);
      }
    }
    lo = p;
  }
};

/// Forward membership probe over a DocSet with the representation resolved
/// once, replacing per-doc virtual DocSet::get() in scan loops: BITSET
/// probes become stateless word tests, ARRAY probes go through an
/// ArrDocSetProbe cursor. A null set accepts every doc.
class DocSetProbe {
  const uint64_t* words = nullptr;
  ArrDocSetProbe arr;

public:
  DocSetProbe() = default;  // accepts everything, like a null DocSet

  explicit DocSetProbe(const DocSet* set) {
    reset(set);
  }

  void reset(const DocSet* set) {
    words = nullptr;
    arr = ArrDocSetProbe();
    if (set == nullptr) {
      return;
    }
    if (set->type == DocSet::BITSET) {
      words = ((const BitDocSet*) set)->bits().words;
    } else {
      arr.reset(*(const ArrDocSet*) set);
      assert(arr.bound());
    }
  }

  bool get(int32_t doc) {
    if (words != nullptr) {
      return (words[(uint32_t) doc >> 6] >> (doc & 63)) & 1;
    }
    if (!arr.bound()) {
      return true;  // no set
    }
    return arr.get(doc);
  }

  /// Invoke callback(doc, index) for every member of the strictly-ascending
  /// span, in span order; a null set matches every element. One representation
  /// dispatch per span instead of per doc. Mark the callback SOLUX_INLINE at
  /// the call site.
  template <class F>
  void intersect(std::span<const int32_t> docs, F&& callback) {
    if (words != nullptr) {
      for (int32_t i = 0; i < (int32_t) docs.size(); i++) {
        int32_t doc = docs[(size_t) i];
        if ((words[(uint32_t) doc >> 6] >> (doc & 63)) & 1) {
          callback(doc, i);
        }
      }
      return;
    }
    if (!arr.bound()) {
      for (int32_t i = 0; i < (int32_t) docs.size(); i++) {
        callback(docs[(size_t) i], i);
      }
      return;
    }
    arr.intersect(docs, std::forward<F>(callback));
  }
};

class DocSetBuilder {
  #ifndef NDEBUG
  int32_t lastDoc = -1;

  void checkMonotonic(int32_t docid) {
    assert(docid >= 0);
    assert(docid < max);
    assert(docid > lastDoc);
    lastDoc = docid;
  }

  void checkWindowWordsMonotonic(const uint64_t* words, int32_t windowStart,
                                 int32_t windowEnd) {
    assert(windowStart >= 0);
    assert(windowEnd >= windowStart);
    assert(windowEnd <= max);
    int32_t nbits = windowEnd - windowStart;
    int32_t nwords = (nbits + 63) >> 6;
    for (int32_t wordIdx = 0; wordIdx < nwords; wordIdx++) {
      uint64_t word = maskedWindowWord(words[wordIdx], wordIdx, nwords, nbits);
      while (word != 0) {
        int32_t bit = (int32_t) std::countr_zero(word);
        checkMonotonic(windowStart + (wordIdx << 6) + bit);
        word &= word - 1;
      }
    }
  }
  #endif

  static uint64_t lowBitsMask(int32_t bits) {
    assert(bits >= 0 && bits <= 64);
    if (bits == 0) return 0;
    if (bits == 64) return ~0ULL;
    return (1ULL << bits) - 1ULL;
  }

  static uint64_t maskedWindowWord(uint64_t word, int32_t wordIdx,
                                   int32_t nwords, int32_t nbits) {
    if (wordIdx + 1 == nwords) {
      int32_t validBits = nbits & 63;
      if (validBits != 0) {
        word &= lowBitsMask(validBits);
      }
    }
    return word;
  }

  void promoteToBits() {
    assert(!bits);
    bitDocs.emplace(max);
    bits = &bitDocs->mutableBits();
    for (auto d : docs) {
      bits->set(d);
    }
    bitDocs->setCard((int32_t) docs.size());
    docs.clear();
    docs.shrink_to_fit();
  }

  void appendWindowDocsToArray(const uint64_t* words, int32_t windowStart,
                               int32_t windowEnd, int32_t maxDocsToAppend) {
    if (maxDocsToAppend <= 0) {
      return;
    }
    int32_t nbits = windowEnd - windowStart;
    int32_t nwords = (nbits + 63) >> 6;
    for (int32_t wordIdx = 0; wordIdx < nwords && maxDocsToAppend > 0; wordIdx++) {
      uint64_t word = maskedWindowWord(words[wordIdx], wordIdx, nwords, nbits);
      while (word != 0 && maxDocsToAppend > 0) {
        int32_t bit = (int32_t) std::countr_zero(word);
        docs.emplace_back(windowStart + (wordIdx << 6) + bit);
        maxDocsToAppend--;
        word &= word - 1;
      }
    }
  }

  void orWindowWordsToBits(const uint64_t* words, int32_t windowStart,
                           int32_t windowEnd, int32_t wordCard) {
    assert(bits != nullptr);
    int32_t nbits = windowEnd - windowStart;
    int32_t nwords = (nbits + 63) >> 6;
    int32_t destWord = windowStart >> 6;
    int32_t shift = windowStart & 63;
    int32_t bitWordCount = (int32_t) FixedBitSet::sizeInWords(max);

    for (int32_t i = 0; i < nwords; i++) {
      uint64_t source = maskedWindowWord(words[i], i, nwords, nbits);
      if (source == 0) {
        continue;
      }

      int32_t lowWord = destWord + i;
      if (lowWord < bitWordCount) {
        uint64_t low = shift == 0 ? source : source << shift;
        bits->words[lowWord] |= low;
      }

      if (shift != 0) {
        int32_t highWord = lowWord + 1;
        if (highWord < bitWordCount) {
          uint64_t high = source >> (64 - shift);
          if (high != 0) {
            bits->words[highWord] |= high;
          }
        }
      }
    }
    bitDocs->card_ += wordCard;
  }

public:
  const int32_t max;
  std::vector<int32_t> docs;
  std::optional<RAMBitDocSet> bitDocs;
  FixedBitSet* bits = nullptr;
  DocSetBuilder(int32_t max) : max(max) {}

  static int32_t arrayLimitFor(int32_t maxDoc) noexcept {
    return (maxDoc + 31) >> 5;
  }

  int32_t arrayLimit() const noexcept {
    return arrayLimitFor(max);
  }

  void add(int32_t docid) SOLUX_INLINE {
    #ifndef NDEBUG
    checkMonotonic(docid);
    #endif
    if (bits) {
      bits->set(docid);
      bitDocs->card_++;
      return;
    }
    if (docs.size() < (size_t) arrayLimit()) {
      docs.emplace_back(docid);
      return;
    }
    promoteToBits();
    bits->set(docid);
    bitDocs->card_++;
  }

  void addSorted(std::span<const int32_t> sortedDocs) {
    if (sortedDocs.empty()) {
      return;
    }
#ifndef NDEBUG
    for (int32_t doc : sortedDocs) {
      checkMonotonic(doc);
    }
#endif
    if (bits) {
      for (int32_t doc : sortedDocs) {
        bits->set(doc);
      }
      bitDocs->card_ += (int32_t) sortedDocs.size();
      return;
    }

    int32_t limit = arrayLimit();
    int32_t appendBeforePromotion = std::min(
        (int32_t) sortedDocs.size(),
        std::max<int32_t>(0, limit - (int32_t) docs.size()));
    docs.insert(docs.end(), sortedDocs.begin(),
                sortedDocs.begin() + appendBeforePromotion);
    if (appendBeforePromotion == (int32_t) sortedDocs.size()) {
      return;
    }

    promoteToBits();
    for (int32_t doc : sortedDocs.subspan((size_t) appendBeforePromotion)) {
      bits->set(doc);
    }
    bitDocs->card_ += (int32_t) sortedDocs.size() - appendBeforePromotion;
  }

  int32_t card() const {
    return bits ? bitDocs->cachedCard() : (int32_t) docs.size();
  }

  void addWindowWords(const uint64_t* words, int32_t windowStart,
                      int32_t windowEnd, int32_t wordCard) {
    assert(words != nullptr);
    assert(windowStart >= 0);
    assert(windowEnd >= windowStart);
    assert(windowEnd <= max);
    assert(wordCard >= 0);
    if (windowEnd <= windowStart) {
      assert(wordCard == 0);
      return;
    }

    if (wordCard == 0) {
      return;
    }

    #ifndef NDEBUG
    checkWindowWordsMonotonic(words, windowStart, windowEnd);
    #endif

    if (bits) {
      orWindowWordsToBits(words, windowStart, windowEnd, wordCard);
      return;
    }

    int32_t limit = arrayLimit();
    if ((int32_t) docs.size() + wordCard <= limit) {
      appendWindowDocsToArray(words, windowStart, windowEnd, wordCard);
      return;
    }

    int32_t appendBeforePromotion = std::max<int32_t>(0, limit - (int32_t) docs.size());
    appendWindowDocsToArray(words, windowStart, windowEnd, appendBeforePromotion);
    promoteToBits();
    orWindowWordsToBits(words, windowStart, windowEnd,
                        wordCard - appendBeforePromotion);
  }

  std::unique_ptr<DocSet> build() {
    if (bitDocs.has_value()) {
      return std::make_unique<RAMBitDocSet>(std::move(*bitDocs));
    }
    docs.shrink_to_fit();
    return std::make_unique<ArrDocSet>(std::move(docs));
  }
};

/// Merge two or more DocSets into a single new DocSet.
inline std::unique_ptr<DocSet> DocSet::intersect(std::span<DocSet*> sets) {
  assert(sets.size() > 1);
  std::sort(sets.begin(), sets.end(), [](DocSet* a, DocSet* b) {
    return a->card() < b->card();
  });

  size_t firstBitset = sets.size();
  bool allBitsets = true;
  for (size_t i = 0; i < sets.size(); i++) {
    if (sets[i]->type == BITSET) {
      if (firstBitset == sets.size()) {
        firstBitset = i;
      }
    } else {
      allBitsets = false;
    }
  }

  int32_t nbits = -1;
  if (firstBitset != sets.size()) {
    nbits = ((BitDocSet*) sets[firstBitset])->bits().size();
    for (DocSet* set : sets) {
      if (set->type == BITSET) {
        if (((BitDocSet*) set)->bits().size() != nbits) {
          throw std::invalid_argument("DocSet bitset sizes differ");
        }
      } else {
        auto docs = ((ArrDocSet*) set)->docs();
        if (!docs.empty() && (docs.front() < 0 || docs.back() >= nbits)) {
          throw std::invalid_argument("DocSet array is outside bitset range");
        }
      }
    }
  }

  if (allBitsets) {
    auto& firstBits = ((BitDocSet*) sets[0])->bits();
    auto nWords = firstBits.sizeInWords(nbits);
    auto result = std::make_unique<RAMBitDocSet>(nbits);
    auto& bits = result->mutableBits();
    memcpy(bits.words, firstBits.words, nWords * sizeof(*bits.words));
    for (size_t i = 1; i < sets.size(); i++) {
      for (size_t word = 0; word < nWords; word++) {
        bits.words[word] &= ((BitDocSet*)sets[i])->bits().words[word];
      }
    }
    return result;
  }

  // Mixed representations use the true smallest-cardinality set as the lead.
  // Iterating it and probing the rest is total for either lead representation.
  // Lead docs ascend, so each probe's array cursor only moves forward.
  std::vector<int32_t> outputDocs;
  outputDocs.reserve((size_t) sets[0]->card());
  std::vector<DocSetProbe> probes(sets.begin() + 1, sets.end());
  auto matchesRest = [&](int32_t doc) {
    for (auto& probe : probes) {
      if (!probe.get(doc)) {
        return false;
      }
    }
    return true;
  };

  if (sets[0]->type == ARRAY) {
    for (int32_t doc : ((ArrDocSet*) sets[0])->docs()) {
      if (matchesRest(doc)) {
        outputDocs.emplace_back(doc);
      }
    }
  } else {
    const FixedBitSet& bits = ((BitDocSet*) sets[0])->bits();
    size_t nWords = FixedBitSet::sizeInWords(bits.size());
    for (size_t wordIdx = 0; wordIdx < nWords; wordIdx++) {
      uint64_t word = bits.words[wordIdx];
      if (wordIdx + 1 == nWords && (bits.size() & 63) != 0) {
        word &= (1ULL << (bits.size() & 63)) - 1ULL;
      }
      while (word != 0) {
        int32_t doc = (int32_t) ((wordIdx << 6) + std::countr_zero(word));
        if (matchesRest(doc)) {
          outputDocs.emplace_back(doc);
        }
        word &= word - 1;
      }
    }
  }

  outputDocs.shrink_to_fit();
  return std::make_unique<ArrDocSet>(std::move(outputDocs));
}

inline std::unique_ptr<DocSet> DocSet::union_(std::span<DocSet*> sets) {
  assert(sets.size() > 1);

  // If any input is BITSET, the result is a BITSET sized to match. Invalid
  // mixed ranges and mismatched BITSET sizes are rejected in every build.
  size_t firstBitset = sets.size();
  for (size_t i = 0; i < sets.size(); i++) {
    if (sets[i]->type == BITSET) {
      firstBitset = i;
      break;
    }
  }

  if (firstBitset < sets.size()) {
    auto& firstBits = ((BitDocSet*)sets[firstBitset])->bits();
    int32_t nbits = firstBits.size();
    auto nWords = firstBits.sizeInWords(nbits);
    auto result = std::make_unique<RAMBitDocSet>(nbits);
    auto& bits = result->mutableBits();
    memcpy(bits.words, firstBits.words, nWords * sizeof(*bits.words));

    for (size_t i = 0; i < sets.size(); i++) {
      if (i == firstBitset) continue;
      if (sets[i]->type == BITSET) {
        auto& other = ((BitDocSet*)sets[i])->bits();
        if (other.size() != nbits) {
          throw std::invalid_argument("DocSet bitset sizes differ");
        }
        for (size_t w = 0; w < nWords; w++) {
          bits.words[w] |= other.words[w];
        }
      } else {
        for (auto doc : ((ArrDocSet*)sets[i])->docs()) {
          if (doc < 0 || doc >= nbits) {
            throw std::invalid_argument("DocSet array is outside bitset range");
          }
          bits.set(doc);
        }
      }
    }
    return result;
  }

  // All ARRAYs -> N-way sort-merge using std::set_union with two scratch
  // buffers, ping-ponging the running output.
  std::vector<int32_t> buf0;
  std::vector<int32_t> buf1;
  std::span<int32_t> a = ((ArrDocSet*)sets[0])->docs();
  std::span<int32_t> b = ((ArrDocSet*)sets[1])->docs();
  std::vector<int32_t>* current = &buf0;
  current->reserve(a.size() + b.size());
  std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(*current));

  std::vector<int32_t>* scratch = &buf1;
  for (size_t i = 2; i < sets.size(); i++) {
    auto next = ((ArrDocSet*)sets[i])->docs();
    scratch->clear();
    scratch->reserve(current->size() + next.size());
    std::set_union(current->begin(), current->end(),
                   next.begin(), next.end(),
                   std::back_inserter(*scratch));
    std::swap(current, scratch);
  }
  current->shrink_to_fit();
  return std::make_unique<ArrDocSet>(std::move(*current));
}


}
