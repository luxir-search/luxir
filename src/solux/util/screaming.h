#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <bit>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <sstream>
#include <assert.h>

#if defined(__BMI2__)
#include <immintrin.h>
#endif


namespace screaming {
// Screaming bitsets... a play on (and inspired by) Roaring Bitmaps

// Design:
//   - Need ability to efficiently find the rank of a value (i.e. the number of values that come before it)
//     Motivation: bitsets are often used with parallel arrays (i.e. doc values, where each entry has a document id
//     represented by the bitset, and then a separate list of dense values.  We need to know the index into those
//     dense values.)
//   - Need to represent "mostly full" cases as efficiently as "mostly empty" cases.
//   - Minimal size for small sets since we could have many sparse fields, or many bitsets in a very small segment.


// Bucket descriptor:
//   2:[high 16 bits]  2:[cardinality-1] 4:[ byte offset to container ]
// if cardinality is max, store nothing (offset is meaningless)
// if cardinality is 1 or 2, we could inline those values in the offset. (set the pointer to the offset itself... no need to change nextValue code)
// if cardinality is 65536-(1 or 2) then we could inline the negative in the offset
// since offsets will always be even, we have a spare bit there if we need it.
//
// Idea: if the bucket list itself is very dense, we could go ahead and make it direct mapped so random lookups would be faster.
// We would just need an extra bucket to signify that it's an empty bucket.
// Provide or deduce size from the reader?


// As in Solr, we need a bitset that doesn't hide its implementation.
// size (number of bits) should be an exact multiple of word_type
// word_type must be an unsigned type.
// Example  OpenBitSet<65536, uint64_t, uint32_t> mySet;
//
// This can also be used as the basis for dynamically sized bitsets, just
// don't use methods that rely on size.
template <uint64_t size_, typename word_type_, typename index_type_>
class OpenBitSet {
public:
  using word_type = word_type_;
  using index_type = index_type_;
  static constexpr uint64_t fixedSize = size_;
  static constexpr index_type sizeInBytes = fixedSize / 8;
  static constexpr index_type fixedNumWords = sizeInBytes / sizeof(word_type);
  // number of bits to shift to get the word index. std::bit_width is 1+log2(x), so sub 1 to get back to log2(x)
  static constexpr uint8_t wordShift = std::bit_width(sizeof(word_type) * 8) - 1; // shift to get word of index
  static constexpr word_type wordMask = sizeof(word_type) * 8 - 1;     // mask to get the bit index within a word
  static constexpr word_type allBitsSet = ~((word_type)(0));
  static constexpr index_type MAX_INDEX = std::numeric_limits<index_type>::max();  // maximum value for the index type
  word_type* words;

  OpenBitSet() = default;  // trivial constructor so this can be part of a union
  OpenBitSet(const OpenBitSet& other) = default;
  OpenBitSet& operator=(const OpenBitSet& other) = default;

  // Create a bitset view over existing memory.
  explicit OpenBitSet(word_type* pointer) : words(pointer) {
  }


  void set(index_type val) {
    assert(val >= 0 && val < fixedSize);
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    words[wordIdx] |= (word_type)1 << bitIdx;
  }

  bool get(index_type val) const {
    assert(val >= 0 && val < fixedSize);
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    return words[wordIdx] & ((word_type)1 << bitIdx);
  }

  // returns 0 or 1
  int getInt(index_type val) const {
    assert(val >= 0 && val < fixedSize);
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    return (words[wordIdx] >> bitIdx) & 0x01;
  }


  // Returns the next set bit or MAX_INDEX if none exists.
  // Assumes that any unused high bits in the last word are cleared.
  // If that is not the case, this can return a value >= size.
  index_type nextSetBit(index_type val, index_type numWords = fixedNumWords) const {
    assert(val >= 0 && val < fixedSize);
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    word_type word = words[wordIdx] >> bitIdx;

    if (word != 0) {
      return  val + std::countr_zero(word);
    }

    for (auto i = wordIdx + 1; i < numWords; i++) {
      word = words[i];
      if (word != 0) {
        auto foundIdx = std::countr_zero(word);
        return (i << wordShift) + foundIdx;
      }
    }

    return MAX_INDEX;
  }


  void clear() {
    std::memset(words, 0, sizeInBytes);
  }

  void flip(index_type numWords = fixedNumWords) {
    for (index_type i = 0; i<fixedNumWords; i++) {
      words[i] = ~words[i];
    }
  }

  // TODO: untested
  // These are static so they can be used more easily in other contexts, with different offsets, etc.
  template <class Callable>
  static void visitZeroes(const word_type* words, index_type numWords, Callable callable) {
    for (index_type i=0; i<numWords; i++) {
      word_type word = ~words[i];  // flip bits and find the ones
      uint8_t bitIdx = 0;
      while (word != 0) {
        auto foundIdx = std::countr_zero(word);
        bitIdx += foundIdx;
        word >>= (bitIdx + 1);
        callable(i * sizeof(word_type) * 8 + bitIdx);
      }
    }
  }

  // TODO: untested
  template <class Callable>
  static void visitOnes(const word_type* words, index_type numWords, Callable callable) {
    for (index_type i=0; i<numWords; i++) {
      word_type word = words[i];
      uint8_t bitIdx = 0;
      while (word != 0) {
        auto foundIdx = std::countr_zero(word);
        bitIdx += foundIdx;
        word >>= (bitIdx + 1);
        callable(i * sizeof(word_type) * 8 + bitIdx);
      }
    }
  }

  // TODO: untested.  This should be a faster way than shifting the word.  Compiler explorer shows that the
  // clearing-the-lowest-bit code is translated to BLSR (x86), which also eliminates a separate test for 0
  template <class Callable>
  static void visitOnes(uint64_t word, Callable callable) {
    while (word != 0) {
      auto foundIdx = std::countr_zero(word);
      word = word & (word - 1); // clears the lowest bit
      callable(foundIdx);
    }
  }

};


/// A bitset with int32_t for indexes with a size that is given at runtime.
/// FixedBitSet is non-owning. It only creates a view over existing memory.
class FixedBitSet : public OpenBitSet<std::numeric_limits<int32_t>::max(), uint64_t, int32_t> {
  using OBS = OpenBitSet;
  const int32_t nbits;
  const uint32_t nwords;
public:
  FixedBitSet(uint64_t* ptr, int32_t nbits) : OpenBitSet(ptr), nbits(nbits), nwords(sizeInWords(nbits)) {
    assert(nbits >= 0 && nbits <= OBS::fixedSize);
    assert(ptr != nullptr || nbits == 0); // if nbits is 0, we can have a null pointer
  }

  /// number of words needed to store nbit bits
  static size_t sizeInWords(int32_t nbits) {
    assert(nbits >= 0);
    // since we went from signed to unsigned, we can add to sz without overflow issues.
    uint32_t numWords = ((uint32_t)nbits + sizeof(uint64_t)*8 - 1) / (sizeof(uint64_t) * 8); // round up to nearest word
    return numWords;
  }

  /// Allocate and return a bitset initialized with initialVal
  static std::unique_ptr<uint64_t[]> allocate(int32_t nbits, bool initialVal=false) {
    if (initialVal == false) {
      // guaranteed value-initialized, so will be all zeroes
      return std::make_unique<uint64_t[]>(sizeInWords(nbits));
    } else {
      // want all 1 bits.
      auto numWords = sizeInWords(nbits);
      auto ptr = std::make_unique_for_overwrite<uint64_t[]>(numWords);
      std::memset(ptr.get(), 0xFF, numWords * sizeof(uint64_t));

      auto usedBits = nbits % (sizeof(uint64_t) * 8);
      // clear the high bits that are not used with a right shift
      if (usedBits > 0) {
        auto unusedBits = sizeof(uint64_t) * 8 - usedBits;
        ptr[numWords - 1] >>= unusedBits;
      }
      return ptr;
    }
  }

  int32_t size() const {
    return nbits;
  }

  int32_t card() const {
    int32_t count = 0;
    for (int32_t i = 0; i < nwords; i++) {
      count += std::popcount(words[i]);
    }
    return count;
  }

  void set(int32_t index) {
    assert(index < nbits);
    OBS::set(index);
  }

  bool get(int32_t index) const {
    assert(index < nbits);
    return OBS::get(index);
  }

  bool operator[](int32_t index) const {
    assert(index < nbits);
    return OBS::get(index);
  }

  bool operator[](size_t index) const {
    return OBS::get((int32_t)index);
  }

  // returns 0 or 1
  int getInt(int32_t index) const {
    assert(index < nbits);
    return OBS::getInt(index);
  }

  // Returns the next set bit starting at the given index, or MAX_INDEX if none exists.
  index_type nextSetBit(int32_t index) const {
    assert(index < nbits);
    return OBS::nextSetBit(index, nwords);
  }

  // Clear a bit at the given index
  void clear(int32_t index) {
    assert(index >= 0 && index < nbits);
    index_type wordIdx = index >> OBS::wordShift;
    uint8_t bitIdx = index & OBS::wordMask;
    words[wordIdx] &= ~((word_type)1 << bitIdx);
  }

  // Returns the next clear bit starting at the given index, or MAX_INDEX if none exists.
  index_type nextClearBit(int32_t index) const {
    assert(index >= 0 && index <= nbits);
    index_type wordIdx = index >> OBS::wordShift;
    uint8_t bitIdx = index & OBS::wordMask;
    
    // Check the current word first, starting from the given bit
    word_type word = ~words[wordIdx];  // invert to find clear bits
    word &= ~((word_type(1) << bitIdx) - 1);  // mask off bits before index
    
    if (word != 0) {
      int32_t result = (wordIdx << OBS::wordShift) + std::countr_zero(word);
      if (result < nbits) {
        return result;
      }
    }
    
    // Check subsequent words
    for (auto i = wordIdx + 1; i < nwords; i++) {
      word = ~words[i];  // invert to find clear bits
      if (word != 0) {
        int32_t result = (i << OBS::wordShift) + std::countr_zero(word);
        if (result < nbits) {
          return result;
        }
      }
    }
    
    return MAX_INDEX;
  }
};


/// An owning implementation of FixedBitSet that allocates its own memory.
class RAMFixedBitSet : public FixedBitSet {
  using OBS = OpenBitSet;
public:
  std::unique_ptr<uint64_t[]> ownedWords;

  RAMFixedBitSet(int32_t nbits, bool initialVal=false) : FixedBitSet(allocate(nbits, initialVal).release(), nbits),
  ownedWords(words) {}
};


// The Screaming Bitset read-only implementation.  Use screaming::Builder to build one.
// This is currently limited to int32 indexes, hence only supports 2^31-1 bits.
// The largest value is reserved for NO_VALUE.
// We use a signed integer here to avoid unsigned bugs and for code that is simpler when
// there is a value below the first "real" value (i.e. iterators can be started at -1)
// and when the sentinel for NO_VALUE is above all other values.
class BitSet {
public:
  static constexpr uint8_t  BUCKET_BITS = 16;
  static constexpr uint32_t BUCKET_SIZE = 1 << BUCKET_BITS; // buckets should be 65536 wide, with all vals sharing top 16 bits
  static constexpr uint32_t BUCKET_MASK_UPPERBITS = 0xFFFFFFFF << BUCKET_BITS; // 0xFFFF0000
  static constexpr uint32_t BUCKET_SPARSE_MAX = BUCKET_SIZE / 8 / sizeof(uint16_t);   // max cardinality to express as sparse bucket
  static constexpr uint32_t BUCKET_NEG_MIN = BUCKET_SIZE - BUCKET_SIZE / 8; // min cardinality to express as neg set
  using Bits = OpenBitSet<BUCKET_SIZE, uint64_t, uint32_t>; // type for the bits only of a bit bucket
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  static constexpr uint32_t RANK_INDEX_SHIFT = 3; // for a dense (bitset) block, store a cumulative popcnt every 8 words
  static constexpr uint32_t RANK_INDEX_ENTRIES = Bits::fixedNumWords >> RANK_INDEX_SHIFT; // uint16 entries in a dense block's rank index
  static constexpr uint32_t RANK_INDEX_SIZE = RANK_INDEX_ENTRIES * sizeof(uint16_t);
  static constexpr uint32_t SPARSE_CONTAINER_SIZE = Bits::sizeInBytes;
  static constexpr uint32_t DENSE_CONTAINER_SIZE = Bits::sizeInBytes + RANK_INDEX_SIZE;

  // A dense bucket stores its bitset words immediately followed by a
  // cumulative-popcount rank index (RANK_INDEX_ENTRIES uint16s; see
  // Builder::flushBucket).  This is the single source of where that index lives:
  // Iterator::rank(), selectInBucket(), and the writer all locate it through here.
  // A pure reinterpret_cast, so it inlines to nothing - no cost to the rank() hot path.
  static const uint16_t* denseRankIndex(const Bits::word_type* words) {
    return reinterpret_cast<const uint16_t*>(words + Bits::fixedNumWords);
  }
  static uint16_t* denseRankIndex(Bits::word_type* words) {
    return reinterpret_cast<uint16_t*>(words + Bits::fixedNumWords);
  }


  typedef struct {
    uint16_t upperBits; // upper 16 bits of all values in this bucket
    uint16_t size;      // cardinality of this bucket minus 1
    uint32_t offset;    // location of the data for this bucket
  } BucketDescriptor;

  const char* start;
  const BucketDescriptor* descriptors;
  uint16_t nBuckets;

  BitSet() {
    // if nBuckets==0, none of the other pointers are touched
    nBuckets = 0;
  }

  explicit BitSet(const void* pointerToEnd) {
    set(pointerToEnd);
    /*
    nBuckets = *((uint16_t*)pointerToEnd - 1);
    descriptors = (BucketDescriptor*)((char*)pointerToEnd - sizeof(uint16_t) - nBuckets * sizeof(BucketDescriptor));
    // the size of all the buckets is the offset of the last bucket plus the size of that bucket
    auto lastDescriptor = descriptors[nBuckets - 1];
    auto sizeOfAllBuckets = lastDescriptor.offset + blockBytes(lastDescriptor);
    start = ((char*)descriptors) - sizeOfAllBuckets;
     */
  }

  void set(const void* pointerToEnd) {
    nBuckets = *((uint16_t*)pointerToEnd - 1);
    descriptors = (BucketDescriptor*)((char*)pointerToEnd - sizeof(uint16_t) - nBuckets * sizeof(BucketDescriptor));
    // the size of all the buckets is the offset of the last bucket plus the size of that bucket
    auto lastDescriptor = descriptors[nBuckets - 1];
    auto sizeOfAllBuckets = lastDescriptor.offset + blockBytes(lastDescriptor);
    start = ((char*)descriptors) - sizeOfAllBuckets;
  }

  bool empty() const {
    return nBuckets == 0;
  }

  class Iterator {
    const BitSet *set;  // could also try making a direct copy so there's no indirection?
    int32_t curr = -1;
    int32_t bucketIdx = -1;
    int32_t bucketBase = -1;
    int32_t bucketSize = 0;
    int32_t bucketRank = 0; // number of values in all previous buckets
    uint16_t lowerBits;

    typedef struct {
      uint16_t *values;
      int32_t index;
    } SparseBucket;

    typedef struct {
      Bits obs;
      // future: try caching currentword
      // Bits::word_type currWord;
      // uint16_t wordIdx;
      // uint8_t wordShift;  // how much currWord as been right shifted already
    } DenseBucket;

    union Bucket {
      SparseBucket sparse;
      DenseBucket bits;

      Bucket() {};
    } bucket;

    // enum that takes 16 bits
    enum BucketType : std::uint8_t {
      SPARSE,
      DENSE,
      NEGATIVE
    };

    BucketType bucketType;

  public:
    Iterator(const BitSet &set) : set(&set) {
      bucketType = SPARSE;
      bucket.sparse.index = -1;
    }

    int32_t val() const {
      return curr;
    }

    int32_t next() {
      switch (bucketType) {
        case SPARSE:
          if (bucket.sparse.index + 1 >= bucketSize) {  // this will trigger on first call
            return nextBucket();
          }
          return sparseNext();
        case DENSE:
          return denseNextMaybe();
      }
    }

    int32_t advance(int32_t target) {
      // If it would make it faster, we could also implement skipping-only iterators where we don't mix next() and advance()
      // We could also build a bucket index if the set will be used many times for skipping.
      // We could also try using interpolation to estimate where the value may lie.
      assert(target > curr);
      int32_t targetBase = target & BUCKET_MASK_UPPERBITS;
      if (targetBase != bucketBase) {
        if (!advanceBucket(targetBase)) {
          return END;
        }
        if (bucketBase > targetBase) {
          // the current block we advanced to is larger than the target, so just return first doc of that block.
          return next();
        }
      }

      // now we are in the current block, but need to advance to the specific target within the block
      switch (bucketType) {
        case SPARSE: {
          // TODO: check first value to speed up some common cases? Or do exponential search?
          // This could be a case for a branchless binary search as well.
          int lowerIdx = bucket.sparse.index + 1;
          auto startPtr = bucket.sparse.values + lowerIdx;
          auto endPtr = bucket.sparse.values + bucketSize;
          uint16_t lowerBits = (uint16_t) target;
          auto lowerBound = std::lower_bound(startPtr, endPtr, lowerBits);
          if (lowerBound == endPtr) {
            // not found, so return the next val in the next bucket.
            return nextBucket();
          }
          bucket.sparse.index = lowerIdx + (lowerBound - startPtr) - 1; // back up one so we can use sparseNext()
          return sparseNext();
        }
        case DENSE:
          curr = target - 1;
          return denseNextMaybe();
      }
      // unreachable
    }


    // only valid if we are in a sparse bucket and there are more values to read in that bucket
    int32_t sparseNext() {
      uint16_t nextInBucket = bucket.sparse.values[++bucket.sparse.index];
      curr = bucketBase + nextInBucket;
      return curr;
    }

    // only valid if we are within a bitset bucket and there are more values
    int32_t denseNext() {
      auto localIndex = uint16_t(curr + 1);
      auto localFound = bucket.bits.obs.nextSetBit(localIndex);
      curr = localFound + bucketBase;
      return curr;
    }

    // only valid if we are within a bitset bucket, but we don't know if there is another value in this bucket
    // and may have to advance to the next bucket to get it.
    int32_t denseNextMaybe() {
      int next = curr + 1;
      if (next < bucketBase + Bits::fixedSize) {
        auto localIndex = uint16_t(next);
        // if localIndex==0 then we wrapped
        auto localFound = bucket.bits.obs.nextSetBit(localIndex);
        if (localFound != Bits::MAX_INDEX) {
          curr = bucketBase + localFound;
          return curr;
        }
      }

      return nextBucket();
    }

    // NOTE: if one is calling next() followed by rank(), it's more efficient to just increment the previous rank by 1.
    // This is only useful in conjunction with advance.
    int32_t rank() {
      // If we always want rank on every advance, we could do this more efficiently by updating the rank
      // in the advance method.

      switch (bucketType) {
        case SPARSE:
          return bucketRank + bucket.sparse.index;
        case DENSE:
          // if bit buckets were 64 byte aligned, then every miniblock of 8 words would be exactly a cache line.
          auto localIndex = uint16_t(curr);
          auto rankIdx = localIndex >> (Bits::wordShift + RANK_INDEX_SHIFT);
          uint16_t* rankIndexArr = denseRankIndex(bucket.bits.obs.words);
          auto rankBase = rankIndexArr[rankIdx];
          // Now find the rank of words before the current word in our mini-block
          auto wordIndex = localIndex >> Bits::wordShift;
          constexpr uint16_t miniBlockMask = (1 << RANK_INDEX_SHIFT) - 1;
          for (auto i = wordIndex & ~miniBlockMask; i < wordIndex; i++) {
            rankBase += std::popcount(bucket.bits.obs.words[i]);
          }
          uint8_t bitIdx = localIndex & Bits::wordMask;
          // shift off our bit and everything higher to get the bits to the right
          // auto bitsToTheRight = bitIdx == 0 ? 0 : (bucket.bits.obs.words[wordIndex] << (sizeof(Bits::word_type)*8 - bitIdx));

          // REPLACEMENT: mask off our bit and all bits higher. This doesn't need a test for 0 and
          // compiles down to a single BZHI instruction (Intel Haswell-2013, AMD Excavator-2015)
          // Another alternative is word & ~(-1 << bitIdx), but it appears that gcc and clang recognize
          // these as equivalent.
          auto bitsToTheRight = bucket.bits.obs.words[wordIndex] & ((Bits::word_type(1) << bitIdx)-1);

          rankBase += std::popcount(bitsToTheRight);
          return bucketRank + rankBase;
      }
      // unreachable
    }


  protected:
    int32_t nextBucket() {
      if (bucketIdx+1 >= set->nBuckets) {
        curr = END;
        return curr;
      }
      bucketRank += bucketSize; // bucketSize is the size of the current bucket, before we advance to the next.

      bucketIdx++;
      const BucketDescriptor& desc = set->descriptors[bucketIdx];
      bucketBase = desc.upperBits << BUCKET_BITS;
      bucketSize = desc.size + 1;
      if (bucketSize <= BUCKET_SPARSE_MAX) {
        bucketType = SPARSE;
        bucket.sparse.index = -1;
        bucket.sparse.values = (uint16_t*)(set->start + desc.offset);
        return sparseNext();
      } else {
        bucketType = DENSE;
        bucket.bits.obs = Bits((Bits::word_type*)(set->start + desc.offset));
        curr = bucketBase - 1;
        return denseNext();
      }
    }

    bool advanceBucket(int32_t targetBase) {
      uint16_t upperBits = targetBase >> BUCKET_BITS;
      for (int i=bucketIdx+1; i<set->nBuckets; i++) {
        bucketRank += bucketSize;
        auto desc = set->descriptors[i];
        bucketSize = desc.size + 1;
        if (desc.upperBits >= upperBits) {
          bucketSetup(i);
          return true;
        }
      }
      return false;
    }

    void bucketSetup(int bucketIndex) {
      bucketIdx = bucketIndex;
      const BucketDescriptor& desc = set->descriptors[bucketIdx];
      bucketBase = desc.upperBits << BUCKET_BITS;
      bucketSize = desc.size + 1;
      if (bucketSize <= BUCKET_SPARSE_MAX) {
        bucketType = SPARSE;
        bucket.sparse.index = -1;
        bucket.sparse.values = (uint16_t*)(set->start + desc.offset);
      } else {
        bucketType = DENSE;
        bucket.bits.obs = Bits((Bits::word_type*)(set->start + desc.offset));
        curr = bucketBase - 1;
      }
    }

  }; // end Iterator

  // Total number of set bits in the whole set, by summing bucket cardinalities
  // (O(nBuckets); there is no stored count or fast path).  Test-only: production
  // never needs a set's cardinality because it is always redundant with other
  // recorded index data (e.g. a field's docsWithField count), which is why we
  // never persist it.
  int32_t cardinality() const {
    int32_t c = 0;
    for (int32_t i = 0; i < nBuckets; i++) c += (int32_t)descriptors[i].size + 1;
    return c;
  }

  // Value of the k-th set bit (0 <= k < cardinality()); the inverse of
  // Iterator::rank().  Stateless: linear-scans the (small, contiguous) bucket
  // descriptors to locate the owning bucket, then selects within it.  For many
  // repeated lookups against one set, use Selector, which amortizes the bucket
  // scan with a precomputed prefix + binary search.
  int32_t select(int32_t k) const {
    assert(k >= 0);
    int32_t cum = 0;
    for (int32_t bi = 0; bi < nBuckets; bi++) {
      int32_t card = (int32_t)descriptors[bi].size + 1;
      if (k < cum + card) return selectInBucket(bi, k - cum);
      cum += card;
    }
    assert(false && "screaming::BitSet::select: k out of range");
    return END;
  }

  // Amortized repeated select() over one set: owns a copy of the (lightweight)
  // BitSet view and a precomputed per-bucket cumulative cardinality prefix, so
  // each select() is a binary search over buckets plus an O(1)/few-word
  // within-bucket select.  Defined out-of-line below (needs a complete BitSet to
  // hold one by value).
  class Selector;

protected:

  // Value of the localRank-th set bit (0-based) within bucket bi.  Implementation
  // detail of select() / Selector::select().
  int32_t selectInBucket(int32_t bi, int32_t localRank) const {
    const BucketDescriptor& desc = descriptors[bi];
    int32_t bucketBase = (int32_t)desc.upperBits << BUCKET_BITS;
    int32_t card = (int32_t)desc.size + 1;
    assert(localRank >= 0 && localRank < card);
    if (card <= (int32_t)BUCKET_SPARSE_MAX) {
      // Sparse bucket: a sorted uint16 array, so the localRank-th value is direct.
      const uint16_t* vals = (const uint16_t*)(start + desc.offset);
      return bucketBase + (int32_t)vals[localRank];
    }
    // Dense bucket: a bitset block followed by the cumulative-popcount rank index
    // (one uint16 per 1<<RANK_INDEX_SHIFT words; entry m == popcount of the words
    // before mini-block m, with the final mini-block intentionally uncounted).
    // Binary-search the index to the owning mini-block, scan its words to the
    // owning word, then select within that word.  Mirrors Iterator::rank().
    const Bits::word_type* words = (const Bits::word_type*)(start + desc.offset);
    const uint16_t* rankIndexArr = denseRankIndex(words);
    // largest m with rankIndexArr[m] <= localRank
    int32_t lo = 0, hi = (int32_t)RANK_INDEX_ENTRIES;
    while (lo < hi) {
      int32_t mid = (lo + hi) >> 1;
      if ((int32_t)rankIndexArr[mid] <= localRank) lo = mid + 1; else hi = mid;
    }
    int32_t wordIdx = (lo - 1) << RANK_INDEX_SHIFT;
    int32_t rankSoFar = (int32_t)rankIndexArr[lo - 1];
    for (;; wordIdx++) {
      int32_t pc = std::popcount(words[wordIdx]);
      if (rankSoFar + pc > localRank) break;
      rankSoFar += pc;
    }
    int32_t bitPos = selectInWord(words[wordIdx], localRank - rankSoFar);
    return bucketBase + (wordIdx << Bits::wordShift) + bitPos;
  }

  // Position (0-based) of the r-th set bit within a 64-bit word.  Requires
  // r < popcount(word).  PDEP deposits a single bit into the r-th set position;
  // the scalar fallback clears the r lowest set bits.
  static int32_t selectInWord(uint64_t word, int32_t r) {
#if defined(__BMI2__)
    return (int32_t)std::countr_zero(_pdep_u64((uint64_t)1 << (unsigned)r, word));
#else
    for (int32_t i = 0; i < r; i++) word &= word - 1;
    return (int32_t)std::countr_zero(word);
#endif
  }

  // The number of bytes taken up by a block.
  // NOTE: this requires knowing if a rank index is being used for dense blocks!
  static constexpr int blockBytes(const BucketDescriptor& desc) {
    int sz = (int)desc.size + 1;
    if (sz <= BUCKET_SPARSE_MAX) {
      return sz * sizeof(uint16_t);
    } else {
      return DENSE_CONTAINER_SIZE;
    }
  }

};


// Out-of-line so the by-value BitSet member has a complete type.  Still a nested
// class of BitSet, so it retains access to BitSet::selectInBucket.  The scratch
// must hold at least s.nBuckets + 1 int32_t's, and the underlying bitset data
// (not the BitSet object, which is copied) must outlive the Selector.
class BitSet::Selector {
  BitSet set;                          // a copy of the view (pointers into the bitset data)
  // bucketStartRank[i] = rank of bucket i's first set bit = set bits in buckets [0, i);
  // last entry is the total cardinality.  select() binary-searches it for the owning bucket.
  std::span<int32_t> bucketStartRank;
public:
  Selector(const BitSet& s, std::span<int32_t> scratch)
    : set(s), bucketStartRank(scratch.first((size_t)s.nBuckets + 1)) {
    bucketStartRank[0] = 0;
    for (int32_t i = 0; i < s.nBuckets; i++)
      bucketStartRank[i + 1] = bucketStartRank[i] + (int32_t)s.descriptors[i].size + 1;
  }

  // Test-only (see BitSet::cardinality); O(1) here since the prefix is prebuilt.
  int32_t cardinality() const { return bucketStartRank.back(); }

  int32_t select(int32_t k) const {
    assert(k >= 0 && k < bucketStartRank.back());
    // largest bi with bucketStartRank[bi] <= k
    int32_t lo = 0, hi = (int32_t)bucketStartRank.size();
    while (lo < hi) {
      int32_t mid = (lo + hi) >> 1;
      if (bucketStartRank[mid] <= k) lo = mid + 1; else hi = mid;
    }
    int32_t bi = lo - 1;
    return set.selectInBucket(bi, k - bucketStartRank[bi]);
  }
};




template <class Derived>
class Builder {
public:
  using Bits = BitSet::Bits;
  static constexpr uint32_t SCRATCH_SIZE = 1024;  // perhaps template on this, or just pass it in as a value... only consulted during flushing a bucket anyway.

protected:
  using BucketDescriptor = BitSet::BucketDescriptor;

  // number of bucket descriptors per scratch buffer

  uint16_t *values;
  Bits bits;
  uint64_t totalCard = 0;   // total cardinality of the set so far (all bits set is 1 bigger than supported in uint32_t)
  uint32_t currBucket = 0;
  uint32_t bucketSize = 0;  // number of values in the current bucket
  uint32_t numBuckets = 0;  // number of flushed buckets
  uint32_t bucketIdx = 0;   // index of the bucket descriptor within the current scratch space

  uint32_t bucketStart = 0;

  // The scratch buffers are only used to store bucket descriptors, not the actual buckets themselves.
  void* startScratch;    // the first scratch buffer
  void* scratch;         // the current scratch buffer
  uint32_t scratchSize;  // size of each scratch buffer
  const uint32_t maxDescriptors;
  // rankIndex was initially a runtime switch per Builder, but this requires the iterators to know if a set was built
  // with a rank index.  Given that the default is only 3% of dense blocks (and nothing otherwise), it doesn't currently
  // seem worth it to make it switchable.
  static constexpr bool rankIndex = true;        // store an index to speed up rank/ordinal calculations

public:
  // buf_sparseContainer and buf_denseContainer should be of size SPARSE_CONTAINER_SIZE and DENSE_CONTAINER_SIZE
  // SPARSE_CONTAINER_SIZE (max) is 8192 bytes, DENSE_CONTAINER_SIZE is 8448 (3.125% bigger) with RANK_INDEX_SHIFT==3
  Builder(void* buf_sparseContainer, void* buf_denseContainer, void* scratchBuf, uint32_t scratchSize) :
         values((uint16_t*)buf_sparseContainer),
         bits((Bits::word_type*)buf_denseContainer),
         startScratch(scratchBuf),
         scratch(scratchBuf),
         scratchSize(scratchSize),
         maxDescriptors((scratchSize - sizeof(char*)) / sizeof(BucketDescriptor))
         {
  }

  uint32_t cardinality() const {
    return totalCard;
  }

  void add(int32_t val) {
    auto bucket = val >> BitSet::BUCKET_BITS;
    if (bucket != currBucket) {
      assert(bucket > currBucket);
      flushBucket();
      currBucket = bucket;
      bucketSize = 0;
    }
    auto lowerBits = (uint16_t)val;
    if (bucketSize >= BitSet::BUCKET_SPARSE_MAX) {
      // start using bitmap instead
      if (bucketSize == BitSet::BUCKET_SPARSE_MAX) {
        bits.clear();
      }
      bits.set(lowerBits);
    } else {
      values[bucketSize] = lowerBits;
    }
    bucketSize++;
    totalCard++;
  }


  // flushes and returns the total size of the compressed bitset
  uint32_t flush() {
    flushBucket();

    uint32_t descriptorStart = bucketStart; // offset of where we are writing descriptors
    uint32_t totalSize = bucketStart;

    // write bucket descriptors
    void* scratchBuf = startScratch;
    for (int nDescriptors = 0; nDescriptors < numBuckets; nDescriptors += maxDescriptors) {
      auto descriptorsInBuffer = std::min(maxDescriptors, numBuckets - nDescriptors);
      uint32_t writeSize = descriptorsInBuffer * sizeof(BucketDescriptor);
      write(scratchBuf, writeSize);
      totalSize += writeSize;
      void* tmp = scratchBuf;
      if (scratchBuf != scratch) {
        // not at the last buffer yet, so follow link at the end of this buffer
        scratchBuf = *(void**)((char*)scratchBuf + scratchSize - sizeof(char*));
      }
      if (tmp != startScratch) {
        // deallocate all but the first start scratch buffer
        static_cast<Derived*>(this)->deallocateScratch(tmp);
      }
    }

    // Last, write the number of buckets.  TODO: what about 65536 buckets?
    // Should this be coded as numBuckets-1 as well?  That means we can't represent 0 buckets.
    // We could also handle by using 65535 to represent both 65535 and 65536 and writing a dummy
    // bucket at the end to distinguish.  Or we could use the passed in cardinality and say that
    // 0 is not a valid set.
    uint16_t shortNumBuckets = numBuckets;
    write(&shortNumBuckets, sizeof(uint16_t));
    totalSize += sizeof(uint16_t);

    return totalSize;
  }


protected:
  void write(void *ptr, uint32_t nbytes) {
    static_cast<Derived*>(this)->writeBytes(ptr, nbytes);
  }

  void flushBucket() {
    if (bucketSize == 0) {
      // this can happen if nothing was added for the first bucket.
      return;
    }

    uint32_t writeSize = 0;

    // find space for our bucket descriptor
    if (bucketIdx >= maxDescriptors) {
      auto oldScratch = scratch;
      scratch = static_cast<Derived*>(this)->allocateScratch();
      *(void**)((char*)oldScratch + scratchSize - sizeof(char*)) = scratch;  // link old block to new block
      bucketIdx = 0;
    }
    BucketDescriptor& desc = ((BucketDescriptor*)scratch)[bucketIdx];
    bucketIdx++;
    desc.upperBits = currBucket;
    desc.size = bucketSize - 1;
    desc.offset = bucketStart;

    if (bucketSize <= BitSet::BUCKET_SPARSE_MAX) {
      // TODO: if (nVals <= 2) {}
      writeSize = bucketSize * sizeof(uint16_t);
      write((void *) values, writeSize);
    } else {
      // fill in the rest of the bits from the buffered values
      for (uint32_t i=0; i < BitSet::BUCKET_SPARSE_MAX; i++) {
        bits.set(values[i]);
      }

      // TODO: store as negative set if too big
      writeSize = Bits::sizeInBytes;

      if (rankIndex) {
        writeSize = BitSet::DENSE_CONTAINER_SIZE;
        uint16_t* rankIndexArr = BitSet::denseRankIndex(bits.words);
        uint16_t cumulativeRank = 0;
        constexpr uint32_t wordsPerCount = (1<<BitSet::RANK_INDEX_SHIFT);
        // The count represents the cumulative popcnt of the *previous* block, i.e. the first is 0.
        // We could get rid of that count, at the cost of extra logic.  For iterators, we could also calculate rank
        // from the nearest count (i.e. not always rounding down), but that logic would likely make things slower
        // rather than faster. POPCNT on modern processors is fast and the extra branch mispredictions
        // and code complexity prob wouldn't be worth it.
        rankIndexArr[0] = 0;
        // Don't count the last block... it would never be used in our current round-down scheme in the iterator.
        // This also prevents cumulativeRank from overflowing 16 bits.  The other way to prevent it is to
        // implement the all-bits-set optimization of skipping the block entirely.
        for (int i=0; i<bits.fixedNumWords - wordsPerCount; i += wordsPerCount) {
          int rank = 0;
          for (int j=i; j < i + wordsPerCount; j++) {
            rank += std::popcount(bits.words[j]);
          }
          cumulativeRank += rank;
          rankIndexArr[(i>>BitSet::RANK_INDEX_SHIFT) + 1] = cumulativeRank;
        }
      }

      write(bits.words, writeSize);
    }

    numBuckets++;
    bucketStart += writeSize;
    bucketSize = 0;
  }


  /* functions that need to be implemented by derived class
  void writeBytes(void* ptr, uint32_t nbytes) {
  }

  // return a new buffer of size scratchSize, used to keep track of bucket info
  // until flush() is called.
  void* allocateScratch() {
  }
  */
};

class StringStreamBuilder : public Builder<StringStreamBuilder> {
  friend class screaming::Builder<StringStreamBuilder>;
  std::pmr::memory_resource* resource;
  std::ostringstream& out;
public:
  explicit StringStreamBuilder(std::ostringstream& target, std::pmr::memory_resource* resource = std::pmr::get_default_resource()) :
          Builder(resource->allocate(BitSet::SPARSE_CONTAINER_SIZE), resource->allocate(BitSet::DENSE_CONTAINER_SIZE), resource->allocate(SCRATCH_SIZE), SCRATCH_SIZE),
          resource(resource),
          out(target)
  {
  }

  ~StringStreamBuilder() {
    resource->deallocate(startScratch, SCRATCH_SIZE);
    resource->deallocate((void *) bits.words, BitSet::DENSE_CONTAINER_SIZE);
    resource->deallocate(values, BitSet::SPARSE_CONTAINER_SIZE);
  }

protected:
  // only for use by base class
  void writeBytes(void *ptr, uint32_t nbytes) {
    out.write((char*)ptr, nbytes);
  }

  void *allocateScratch() {
    return resource->allocate(scratchSize);
  }

  void deallocateScratch(void* ptr) {
    resource->deallocate(ptr, scratchSize);
  }

};




}