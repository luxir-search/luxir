// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <bit>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <sstream>
#include <utility>
#include <assert.h>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

#include "luxir/util/BranchlessSearch.h"


namespace screaming {
// Screaming bitsets... a play on (and inspired by) Roaring Bitmaps

// Galloping (exponential) lower_bound over [lo, hi): probe lo[1], lo[2], lo[4]...
// until we bracket key, then binary-search the bracket.  Returns the first
// element >= key (== std::lower_bound), but adapts to how far ahead key sits:
// O(log distance) instead of O(log n).  This is what the SPARSE advance wants -
// a forward iterator skipping by small amounts finds the target right next to
// the cursor, touching only nearby (warm) lines.  ~3x faster than a full binary
// search for small skips (the common case, since small skips mean more advances).
template <typename T>
const T* gallopLowerBound(const T* lo, const T* hi, const T& key) noexcept {
  size_t n = (size_t)(hi - lo);
  if (n == 0) return lo;
  size_t bound = 1;
  while (bound < n && lo[bound] < key) bound <<= 1;
  size_t loIdx = bound >> 1;                  // last offset known < key (or 0)
  size_t hiIdx = bound < n ? bound + 1 : n;   // bracket end (exclusive)
  return std::lower_bound(lo + loIdx, lo + hiIdx, key);
}

// Index/projection form for monotonic encoded sources that do not expose a
// contiguous value array. The caller can carry the returned index forward for
// the next increasing key, preserving the same O(log distance) behavior.
template <typename T, typename ValueAt>
int64_t gallopLowerBound(int64_t lo, int64_t hi, const T& key,
                         ValueAt&& valueAt) {
  if (lo >= hi) return lo;
  if (valueAt(lo) >= key) return lo;

  int64_t lastLess = lo;
  int64_t distance = hi - lo;
  int64_t step = 1;
  while (step < distance) {
    int64_t probe = lo + step;
    if (valueAt(probe) >= key) break;
    lastLess = probe;
    if (step > distance / 2) {
      step = distance;
    } else {
      step <<= 1;
    }
  }

  int64_t first = lastLess + 1;
  int64_t end = step < distance ? lo + step + 1 : hi;
  while (first < end) {
    int64_t mid = first + ((end - first) >> 1);
    if (valueAt(mid) < key) first = mid + 1;
    else end = mid;
  }
  return first;
}

// Design:
//   - Need ability to efficiently find the rank of a value (i.e. the number of values that come before it)
//     Motivation: bitsets are often used with parallel arrays (i.e. doc values, where each entry has a document id
//     represented by the bitset, and then a separate list of dense values.  We need to know the index into those
//     dense values.)
//   - Need to represent "mostly full" cases as efficiently as "mostly empty" cases.
//   - Minimal size for small sets since we could have many sparse fields, or many bitsets in a very small segment.


// Bucket descriptor:
//   2:[high 16 bits]  2:[cardinality-1] 4:[ byte offset to container | kind flag ]
// Container offsets are multiples of BLOCK_ALIGN, so the low bits of the offset
// are free; bit 0 (RUN_FLAG) marks a run container.  Each bucket is one of:
//   SPARSE:   card <= 4096, sorted uint16 values.
//   DENSE:    card > 4096, an 8 KB bitmap followed by a rank index.
//   RUN:      RUN_FLAG set, any card < 65536.  nRuns uint16 run starts, then
//             nRuns uint16 cumulative cardinalities before each run (so rank is
//             arithmetic, no summing of run lengths), zero padding, and nRuns as
//             the block's last uint16 (read via the next block's offset).  A
//             run's length is the next run's cumulative count (or the bucket
//             card for the last run) minus its own.
//   ALL_ONES: card == 65536, no payload.
// The builder picks the smallest container per bucket (Roaring's rule) and
// keeps the array/bitmap container on ties.
// if cardinality is 1 or 2, we could inline those values in the offset. (set the pointer to the offset itself... no need to change nextValue code)
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

  // These are static so they can be used more easily in other contexts, with different offsets, etc.
  // Clearing the lowest set bit compiles to BLSR (x86) and avoids shift-count
  // bookkeeping (a >>= 64 on a full word would be UB).
  template <class Callable>
  static void visitZeroes(const word_type* words, index_type numWords, Callable callable) {
    for (index_type i=0; i<numWords; i++) {
      word_type word = ~words[i];  // flip bits and find the ones
      index_type base = i * (index_type)(sizeof(word_type) * 8);
      while (word != 0) {
        auto foundIdx = std::countr_zero(word);
        word &= word - 1;  // clear the lowest set bit
        callable(base + (index_type)foundIdx);
      }
    }
  }

  template <class Callable>
  static void visitOnes(const word_type* words, index_type numWords, Callable callable) {
    for (index_type i=0; i<numWords; i++) {
      word_type word = words[i];
      index_type base = i * (index_type)(sizeof(word_type) * 8);
      while (word != 0) {
        auto foundIdx = std::countr_zero(word);
        word &= word - 1;  // clear the lowest set bit
        callable(base + (index_type)foundIdx);
      }
    }
  }

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

  // OR [from, to) into destination with bit 0 corresponding to from.
  // The final partial word is masked; destination words beyond the range are
  // untouched.
  void orRange(std::span<uint64_t> destination, int32_t from,
               int32_t to) const {
    assert(from >= 0 && to >= from && to <= nbits);
    assert((int64_t) destination.size() * 64 >= to - from);
    if (from == to) return;
    if (((from | to) & 63) != 0) {
      orUnalignedRange(destination, from, to);
      return;
    }
    const uint64_t* source = words + (from >> 6);
    int32_t count = (to - from) >> 6;
    for (int32_t i = 0; i < count; i++) destination[(size_t) i] |= source[i];
  }

private:
  // Whole-word windows are the common path. Keep arbitrary shifts and the
  // tail mask out of their callers' register frame and inline budget.
  [[gnu::noinline]] void orUnalignedRange(std::span<uint64_t> destination,
                                        int32_t from, int32_t to) const {
    assert(from >= 0 && to >= from && to <= nbits);
    int32_t bitCount = to - from;
    assert((int64_t) destination.size() * 64 >= bitCount);
    if (bitCount == 0) return;

    int32_t fullWords = bitCount >> 6;
    int32_t sourceWord = from >> 6;
    int32_t shift = from & 63;
    const uint64_t* source = words + sourceWord;
    // Keep the tail mask out of the word loop. A conditional on the last
    // word makes vectorization depend on matching source/destination
    // alignment, which changes with unrelated arena allocations.
    if (shift == 0) {
      for (int32_t i = 0; i < fullWords; i++) {
        destination[(size_t) i] |= source[i];
      }
    } else {
      for (int32_t i = 0; i < fullWords; i++) {
        destination[(size_t) i] |= (source[i] >> shift)
            | (source[i + 1] << (64 - shift));
      }
    }
    int32_t tailBits = bitCount & 63;
    if (tailBits != 0) {
      uint64_t sourceBits = source[fullWords] >> shift;
      if (shift != 0 && sourceWord + fullWords + 1 < (int32_t) nwords) {
        sourceBits |= source[fullWords + 1] << (64 - shift);
      }
      destination[(size_t) fullWords] |= sourceBits & ((1ULL << tailBits) - 1);
    }
  }

public:

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

  // Bounded navigation must not scan an empty suffix beyond the caller's
  // window. The last word may straddle the limit; mask its result logically.
  index_type nextSetBit(int32_t index, int32_t limit) const {
    assert(index >= 0 && limit <= nbits);
    if (index >= limit) return OBS::MAX_INDEX;
    index_type found = OBS::nextSetBit(index, ((int64_t)limit + 63) >> 6);
    return found < (index_type)limit ? found : OBS::MAX_INDEX;
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
  using Bits = OpenBitSet<BUCKET_SIZE, uint64_t, uint32_t>; // type for the bits only of a bit bucket
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  static constexpr uint32_t RANK_INDEX_SHIFT = 3; // for a dense (bitset) block, store a cumulative popcnt every 8 words
  static constexpr uint32_t RANK_INDEX_ENTRIES = Bits::fixedNumWords >> RANK_INDEX_SHIFT; // uint16 entries in a dense block's rank index
  static constexpr uint32_t RANK_INDEX_SIZE = RANK_INDEX_ENTRIES * sizeof(uint16_t);
  static constexpr uint32_t SPARSE_CONTAINER_SIZE = Bits::sizeInBytes;
  static constexpr uint32_t DENSE_CONTAINER_SIZE = Bits::sizeInBytes + RANK_INDEX_SIZE;

  // Serialized-format alignment invariant: the set's first byte must sit at an
  // 8-aligned address (writers pad their stream/buffer before the builder's
  // first byte), and every bucket block occupies a multiple of 8 bytes (the
  // builder pads sparse blocks; dense blocks are 8448 bytes).  Dense-bucket
  // words are therefore always 8-aligned in memory, which the compiler is
  // entitled to assume when it vectorizes word scans: GCC's AVX-512 nextSetBit
  // loop derives its aligned vector loads from uint64_t alignment, and a
  // byte-misaligned words pointer faults (#GP) - not just a slow path.  The
  // descriptor array lands 8-aligned by the same block padding.
  static constexpr uint32_t BLOCK_ALIGN = 8;
  static_assert(DENSE_CONTAINER_SIZE % BLOCK_ALIGN == 0,
                "dense blocks must preserve the block alignment invariant");

  // Low bit of BucketDescriptor::offset that marks a run container.
  static constexpr uint32_t RUN_FLAG = 1;
  static_assert(RUN_FLAG < BLOCK_ALIGN, "the kind flag must fit below the block alignment");
  // The builder stages a run container's two uint16 arrays in one 8 KB
  // container buffer, which caps the run count.  At the cap a run container
  // is within 3% of a bitmap's size, so the cap only drops near-ties.
  static constexpr uint32_t RUN_MAX = SPARSE_CONTAINER_SIZE / (2 * sizeof(uint16_t));

  enum Kind : uint8_t {
    SPARSE,
    DENSE,
    RUN,
    ALL_ONES
  };

  // Bytes of a run container with nRuns runs, including padding.
  static constexpr uint32_t runBlockBytes(uint32_t nRuns) {
    return (nRuns * 2 * sizeof(uint16_t) + sizeof(uint16_t) + BLOCK_ALIGN - 1) & ~(BLOCK_ALIGN - 1);
  }

  // Bytes of a sparse (array) container with card values, including padding.
  static constexpr uint32_t sparseBlockBytes(uint32_t card) {
    return (card * sizeof(uint16_t) + BLOCK_ALIGN - 1) & ~(BLOCK_ALIGN - 1);
  }

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
    uint32_t offset;    // location of the data for this bucket, ORed with RUN_FLAG for a run container
  } BucketDescriptor;

  // A full bucket of 65536 values needs no container; its rank is arithmetic.
  // Iterators treat it as a run container holding this one run.
  static constexpr uint16_t ALL_ONES_RUN[2] = {0, 0};  // start 0, cumulative 0

  const char* start;
  const BucketDescriptor* descriptors;
  uint16_t nBuckets;

  static constexpr Kind kindOf(const BucketDescriptor& desc) {
    if (desc.offset & RUN_FLAG) return RUN;
    uint32_t card = (uint32_t)desc.size + 1;
    if (card <= BUCKET_SPARSE_MAX) return SPARSE;
    return card == BUCKET_SIZE ? ALL_ONES : DENSE;
  }

  Kind bucketKind(int32_t bi) const {
    return kindOf(descriptors[bi]);
  }

  const char* container(const BucketDescriptor& desc) const {
    return start + (desc.offset & ~(BLOCK_ALIGN - 1));
  }

  // Run count of the run container in bucket bi.  The count is the block's
  // last uint16, so it sits just before the next block (or the descriptors).
  uint32_t runCount(int32_t bi) const {
    const char* end = bi + 1 < nBuckets ? container(descriptors[bi + 1]) : (const char*)descriptors;
    return ((const uint16_t*)end)[-1];
  }

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
    if (nBuckets == 0) {
      start = (const char*)descriptors;
      return;
    }
    // the size of all the buckets is the offset of the last bucket plus the size of that bucket
    auto lastDescriptor = descriptors[nBuckets - 1];
    uint32_t lastBytes = kindOf(lastDescriptor) == RUN
        ? runBlockBytes(((const uint16_t*)descriptors)[-1])
        : blockBytes(lastDescriptor);
    auto sizeOfAllBuckets = (lastDescriptor.offset & ~(BLOCK_ALIGN - 1)) + lastBytes;
    start = ((char*)descriptors) - sizeOfAllBuckets;
    assert((reinterpret_cast<uintptr_t>(start) & (BLOCK_ALIGN - 1)) == 0);
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

    // Also used for ALL_ONES buckets, as one run over ALL_ONES_RUN.
    typedef struct {
      const uint16_t *starts;  // run starts; the cumulative counts follow at starts + nRuns
      int32_t nRuns;
      int32_t index;           // current run, -1 before the first
      int32_t end;             // exclusive absolute end of the current run
    } RunBucket;

    union Bucket {
      SparseBucket sparse;
      DenseBucket bits;
      RunBucket run;

      Bucket() : sparse{nullptr, -1} {}

      void setSparse(uint16_t* values) {
        std::construct_at(&sparse, SparseBucket{values, -1});
      }

      void setDense(Bits::word_type* words) {
        std::construct_at(&bits, DenseBucket{Bits(words)});
      }

      void setRun(const uint16_t* starts, int32_t nRuns, int32_t end) {
        std::construct_at(&run, RunBucket{starts, nRuns, -1, end});
      }
    } bucket;

    // enum that takes 16 bits
    enum BucketType : std::uint8_t {
      SPARSE,
      DENSE,
      RUN
    };

    BucketType bucketType;

  public:
    Iterator(const BitSet &set) : set(&set) {
      bucketType = SPARSE;
      bucket.sparse.index = -1;
    }

    [[gnu::cold]] void reset() {
      curr = bucketIdx = bucketBase = -1;
      bucketSize = bucketRank = 0;
      lowerBits = 0;
      bucketType = SPARSE;
      std::construct_at(&bucket.sparse, SparseBucket{nullptr, -1});
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
        case RUN:
          if (curr + 1 < bucket.run.end) {
            return ++curr;
          }
          return runNextRun();
      }
      std::unreachable();
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
          // Galloping search from the cursor: a forward iterator usually skips a
          // small distance, so the target sits next to startPtr and gallop finds
          // it in O(log skip) touching only warm nearby lines (see gallopLowerBound).
          int lowerIdx = bucket.sparse.index + 1;
          auto startPtr = bucket.sparse.values + lowerIdx;
          auto endPtr = bucket.sparse.values + bucketSize;
          uint16_t lowerBits = (uint16_t) target;
          auto lowerBound = gallopLowerBound(startPtr, endPtr, lowerBits);
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
        case RUN:
          if (target < bucket.run.end) {
            // still inside the current run (target > curr >= its start)
            curr = target;
            return curr;
          }
          return runAdvance(target);
      }
      std::unreachable();
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

    // Moves to the first value of the next run, or the next bucket.
    int32_t runNextRun() {
      int32_t i = bucket.run.index + 1;
      if (i >= bucket.run.nRuns) {
        return nextBucket();
      }
      enterRun(i);
      curr = bucketBase + bucket.run.starts[i];
      return curr;
    }

    // target is in this run bucket and at or past the current run's end.
    // Gallop the run starts from the cursor for the last run starting at or
    // before target.
    int32_t runAdvance(int32_t target) {
      int32_t from = bucket.run.index + 1;
      const uint16_t* starts = bucket.run.starts;
      const uint16_t* endPtr = starts + bucket.run.nRuns;
      auto lowerBits = (uint16_t) target;
      int32_t i = (int32_t)(gallopLowerBound(starts + from, endPtr, lowerBits) - starts);
      if (i < bucket.run.nRuns && starts[i] == lowerBits) {
        enterRun(i);
        curr = target;
        return curr;
      }
      if (i > from) {
        // run i-1 starts before target; it may still cover it
        enterRun(i - 1);
        if (target < bucket.run.end) {
          curr = target;
          return curr;
        }
      }
      if (i < bucket.run.nRuns) {
        enterRun(i);
        curr = bucketBase + starts[i];
        return curr;
      }
      return nextBucket();
    }

    void enterRun(int32_t i) {
      const uint16_t* cums = bucket.run.starts + bucket.run.nRuns;
      int32_t nextCum = i + 1 < bucket.run.nRuns ? cums[i + 1] : bucketSize;
      bucket.run.index = i;
      bucket.run.end = bucketBase + bucket.run.starts[i] + nextCum - cums[i];
    }

    // NOTE: if one is calling next() followed by rank(), it's more efficient to just increment the previous rank by 1.
    // This is only useful in conjunction with advance.
    int32_t rank() {
      // If we always want rank on every advance, we could do this more efficiently by updating the rank
      // in the advance method.

      switch (bucketType) {
        case SPARSE:
          return bucketRank + bucket.sparse.index;
        case RUN: {
          // values before the run, plus the offset within it
          int32_t i = bucket.run.index;
          const uint16_t* cums = bucket.run.starts + bucket.run.nRuns;
          return bucketRank + cums[i] + (curr - bucketBase - bucket.run.starts[i]);
        }
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
      std::unreachable();
    }


  protected:
    int32_t nextBucket() {
      if (bucketIdx+1 >= set->nBuckets) {
        curr = END;
        return curr;
      }
      bucketRank += bucketSize; // bucketSize is the size of the current bucket, before we advance to the next.
      bucketSetup(bucketIdx + 1);
      switch (bucketType) {
        case SPARSE: return sparseNext();
        case DENSE: return denseNext();
        case RUN: return runNextRun();
      }
      std::unreachable();
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

    // Enters bucket bucketIndex positioned before its first value.
    void bucketSetup(int bucketIndex) {
      bucketIdx = bucketIndex;
      const BucketDescriptor& desc = set->descriptors[bucketIdx];
      bucketBase = desc.upperBits << BUCKET_BITS;
      bucketSize = desc.size + 1;
      curr = bucketBase - 1;
      const char* data = set->container(desc);
      switch (kindOf(desc)) {
        case Kind::SPARSE:
          bucketType = SPARSE;
          bucket.setSparse((uint16_t*)data);
          return;
        case Kind::DENSE:
          bucketType = DENSE;
          assert((reinterpret_cast<uintptr_t>(data) & (BLOCK_ALIGN - 1)) == 0);
          bucket.setDense((Bits::word_type*)data);
          return;
        case Kind::RUN:
          bucketType = RUN;
          bucket.setRun((const uint16_t*)data, (int32_t)set->runCount(bucketIdx), bucketBase);
          return;
        case Kind::ALL_ONES:
          bucketType = RUN;
          bucket.setRun(ALL_ONES_RUN, 1, bucketBase);
          return;
      }
      std::unreachable();
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
    switch (kindOf(desc)) {
      case SPARSE: {
        // Sparse bucket: a sorted uint16 array, so the localRank-th value is direct.
        const uint16_t* vals = (const uint16_t*)container(desc);
        return bucketBase + (int32_t)vals[localRank];
      }
      case ALL_ONES:
        return bucketBase + localRank;
      case RUN: {
        // The owning run is the last one whose cumulative count is <= localRank.
        int32_t nRuns = (int32_t)runCount(bi);
        const uint16_t* starts = (const uint16_t*)container(desc);
        const uint16_t* cums = starts + nRuns;
        auto* up = luxir::BranchlessIndex<uint16_t>::upperBound(cums, (size_t)nRuns, localRank);
        int32_t r = (int32_t)(up - cums) - 1;
        return bucketBase + (int32_t)starts[r] + localRank - (int32_t)cums[r];
      }
      case DENSE:
        break;
    }
    // Dense bucket: a bitset block followed by the cumulative-popcount rank index
    // (one uint16 per 1<<RANK_INDEX_SHIFT words; entry m == popcount of the words
    // before mini-block m, with the final mini-block intentionally uncounted).
    // Binary-search the index to the owning mini-block, scan its words to the
    // owning word, then select within that word.  Mirrors Iterator::rank().
    assert((reinterpret_cast<uintptr_t>(container(desc)) & (BLOCK_ALIGN - 1)) == 0);
    const Bits::word_type* words = (const Bits::word_type*)container(desc);
    const uint16_t* rankIndexArr = denseRankIndex(words);
    // largest m with rankIndexArr[m] <= localRank == (first m with rankIndexArr[m] > localRank) - 1
    auto* up = luxir::BranchlessIndex<uint16_t>::upperBound(rankIndexArr, RANK_INDEX_ENTRIES, localRank);
    int32_t lo = (int32_t)(up - rankIndexArr);
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

  // The number of bytes taken up by a non-run block, including its alignment
  // padding (every block occupies a multiple of BLOCK_ALIGN bytes - see the
  // invariant comment above; the builder's flushBucket writes matching padding).
  // A run block's size depends on its run count; see runBlockBytes().
  // NOTE: this requires knowing if a rank index is being used for dense blocks!
  static constexpr uint32_t blockBytes(const BucketDescriptor& desc) {
    switch (kindOf(desc)) {
      case SPARSE: return sparseBlockBytes((uint32_t)desc.size + 1);
      case DENSE: return DENSE_CONTAINER_SIZE;
      case ALL_ONES: return 0;
      case RUN: break;
    }
    assert(false && "run block size needs its run count");
    return 0;
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
  // Precomputed monobound layout for bucketStartRank; select() is called
  // repeatedly against this one array, so remembering it beats recomputing.
  luxir::BranchlessIndex<int32_t> bucketIndex;
public:
  Selector(const BitSet& s, std::span<int32_t> scratch)
    : set(s), bucketStartRank(scratch.first((size_t)s.nBuckets + 1)),
      bucketIndex((size_t)s.nBuckets + 1) {
    bucketStartRank[0] = 0;
    for (int32_t i = 0; i < s.nBuckets; i++)
      bucketStartRank[i + 1] = bucketStartRank[i] + (int32_t)s.descriptors[i].size + 1;
  }

  // Test-only (see BitSet::cardinality); O(1) here since the prefix is prebuilt.
  int32_t cardinality() const { return bucketStartRank.back(); }

  int32_t select(int32_t k) const {
    assert(k >= 0 && k < bucketStartRank.back());
    // largest bi with bucketStartRank[bi] <= k == (first bi with bucketStartRank[bi] > k) - 1
    auto* up = bucketIndex.upperBound(bucketStartRank.data(), k);
    int32_t bi = (int32_t)(up - bucketStartRank.data()) - 1;
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

    // Pick the smallest container (Roaring's rule), keeping the array or
    // bitmap on ties since they iterate without the run bookkeeping.
    if (bucketSize == BitSet::BUCKET_SIZE) {
      // ALL_ONES: the descriptor alone says it all
      writeSize = 0;
    } else if (bucketSize <= BitSet::BUCKET_SPARSE_MAX) {
      uint32_t nRuns = sparseRunCount();
      if (BitSet::runBlockBytes(nRuns) < BitSet::sparseBlockBytes(bucketSize)) {
        // the bitmap buffer is unused for a sparse bucket, so stage the runs there
        uint16_t* runs = (uint16_t*)bits.words;
        sparseRuns(runs, nRuns);
        writeSize = writeRuns(runs, nRuns);
        desc.offset |= BitSet::RUN_FLAG;
      } else {
        // TODO: if (nVals <= 2) {}
        writeSize = bucketSize * sizeof(uint16_t);
        write((void *) values, writeSize);
        // Pad every block to BLOCK_ALIGN so dense-bucket words (and the trailing
        // descriptor array) stay 8-aligned; blockBytes() reports the padded size.
        uint32_t pad = BitSet::sparseBlockBytes(bucketSize) - writeSize;
        if (pad > 0) {
          write((void *) zeros, pad);
          writeSize += pad;
        }
      }
    } else {
      // fill in the rest of the bits from the buffered values
      for (uint32_t i=0; i < BitSet::BUCKET_SPARSE_MAX; i++) {
        bits.set(values[i]);
      }

      uint32_t nRuns = denseRunCount();
      if (nRuns <= BitSet::RUN_MAX && BitSet::runBlockBytes(nRuns) < BitSet::DENSE_CONTAINER_SIZE) {
        // the values have been copied into the bitmap, so stage the runs there
        denseRuns(values, nRuns);
        writeSize = writeRuns(values, nRuns);
        desc.offset |= BitSet::RUN_FLAG;
      } else {
        writeSize = denseWrite();
      }
    }

    numBuckets++;
    bucketStart += writeSize;
    bucketSize = 0;
  }

  static constexpr char zeros[BitSet::BLOCK_ALIGN] = {};

  // Writes the bitmap container plus its rank index.
  uint32_t denseWrite() {
    uint32_t writeSize = Bits::sizeInBytes;

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
      // This also prevents cumulativeRank from overflowing 16 bits (a full bucket is ALL_ONES and has no block).
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
    return writeSize;
  }

  uint32_t sparseRunCount() const {
    uint32_t nRuns = 1;
    for (uint32_t i = 1; i < bucketSize; i++) {
      nRuns += (int32_t)values[i] != (int32_t)values[i - 1] + 1;
    }
    return nRuns;
  }

  // A run starts at each set bit whose lower neighbor is clear.
  uint32_t denseRunCount() const {
    uint32_t nRuns = 0;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < Bits::fixedNumWords; i++) {
      uint64_t word = bits.words[i];
      nRuns += std::popcount(word & ~((word << 1) | carry));
      carry = word >> 63;
    }
    return nRuns;
  }

  // Fills runs[0, nRuns) with run starts and runs[nRuns, 2 * nRuns) with the
  // number of values before each run.
  void sparseRuns(uint16_t* runs, uint32_t nRuns) const {
    uint32_t k = 0;
    for (uint32_t i = 0; i < bucketSize; i++) {
      if (i == 0 || (int32_t)values[i] != (int32_t)values[i - 1] + 1) {
        runs[k] = values[i];
        runs[nRuns + k] = (uint16_t)i;
        k++;
      }
    }
    assert(k == nRuns);
  }

  void denseRuns(uint16_t* runs, uint32_t nRuns) const {
    uint32_t k = 0, rank = 0, runStart = 0;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < Bits::fixedNumWords; i++) {
      uint64_t word = bits.words[i];
      uint64_t lower = (word << 1) | carry;  // bit j holds bit j-1
      uint64_t startBits = word & ~lower;
      uint64_t endBits = ~word & lower;       // first clear bit after a run
      carry = word >> 63;
      // starts and ends alternate in bit order
      for (uint64_t events = startBits | endBits; events != 0; events &= events - 1) {
        uint32_t pos = (i << Bits::wordShift) + (uint32_t)std::countr_zero(events);
        if (startBits & events & (~events + 1)) {
          runStart = pos;
          runs[k] = (uint16_t)pos;
          runs[nRuns + k] = (uint16_t)rank;
          k++;
        } else {
          rank += pos - runStart;
        }
      }
    }
    assert(k == nRuns);
  }

  // Writes a run container staged by sparseRuns/denseRuns: both arrays, zero
  // padding, then the run count as the block's last uint16.
  uint32_t writeRuns(uint16_t* runs, uint32_t nRuns) {
    uint32_t arrayBytes = nRuns * 2 * sizeof(uint16_t);
    uint32_t blockBytes = BitSet::runBlockBytes(nRuns);
    write((void *) runs, arrayBytes);
    uint32_t pad = blockBytes - arrayBytes - sizeof(uint16_t);
    if (pad > 0) {
      write((void *) zeros, pad);
    }
    uint16_t count = (uint16_t)nRuns;
    write(&count, sizeof(uint16_t));
    return blockBytes;
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

// Construct storage before the Builder view, and release every allocation even
// when construction or flush() throws. Descriptor blocks can outlive flush().
class StringStreamStorage {
protected:
  std::pmr::monotonic_buffer_resource storage;
  explicit StringStreamStorage(std::pmr::memory_resource* upstream) : storage(upstream) {}
};

class StringStreamBuilder : private StringStreamStorage, public Builder<StringStreamBuilder> {
  friend class screaming::Builder<StringStreamBuilder>;
  std::ostringstream& out;
public:
  explicit StringStreamBuilder(std::ostringstream& target, std::pmr::memory_resource* resource = std::pmr::get_default_resource()) :
          StringStreamStorage(resource),
          Builder(storage.allocate(BitSet::SPARSE_CONTAINER_SIZE), storage.allocate(BitSet::DENSE_CONTAINER_SIZE), storage.allocate(SCRATCH_SIZE), SCRATCH_SIZE),
          out(target)
  {
  }

protected:
  // only for use by base class
  void writeBytes(void *ptr, uint32_t nbytes) {
    out.write((char*)ptr, nbytes);
  }

  void *allocateScratch() {
    return storage.allocate(scratchSize);
  }

  // The storage owner reclaims these even if flush() never completes.
  void deallocateScratch(void*) {}

};




}
