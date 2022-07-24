#pragma once

#include <cstdint>
#include <vector>
#include <bit>
#include <cstring>
#include <assert.h>


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
// word_type and index_type must be unsigned types.
// Example  OpenBitSet<65536, uint64_t, uint32_t> mySet;
//
// As no bounds checking is done, this can also be used as the basis for dynamically sized bitsets
// (just avoid using any methods that rely on size)
template <uint64_t size_, typename word_type_, typename index_type_>
class OpenBitSet {
public:
  using word_type = word_type_;
  using index_type = index_type_;
  static constexpr uint64_t size = size_;
  static constexpr index_type sizeInBytes = size / 8;
  static constexpr index_type numWords = sizeInBytes / sizeof(word_type);
  // number of bits to shift to get the word index. std::bit_width is 1+log2(x), so sub 1 to get back to log2(x)
  static constexpr uint8_t wordShift = std::bit_width(sizeof(word_type) * 8) - 1;
  static constexpr word_type wordMask = sizeof(word_type) * 8 - 1;     // mask to get the index within a word
  static constexpr word_type allBitsSet = ~((word_type)(0));

  word_type* words;

  // Create a bitset view over existing memory.
  explicit OpenBitSet(word_type* pointer) : words(pointer) {
  }

  void set(index_type val) {
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    words[wordIdx] |= (1 << bitIdx);
  }

  bool get(index_type val) {
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    return words[wordIdx] & (1 << bitIdx);
  }

  // returns 0 or 1
  int getInt(index_type val) {
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    return (words[wordIdx] >> bitIdx) & 0x01;
  }

  void clear() {
    std::memset(words, 0, sizeInBytes);
  }

  void flip() {
    for (index_type i = 0; i<numWords; i++) {
      words[i] = ~words[i];
    }
  }

  // These are static so they can be used more easily in other contexts, with different offsets, etc.
  template <class Callable>
  static void visitZeroes(word_type* words, index_type numWords, Callable callable) {
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

  template <class Callable>
  static void visitOnes(word_type* words, index_type numWords, Callable callable) {
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

};


// The Screaming Bitset read-only implementation.  Use screaming::Builder to build one.
// Hmmm, we could also allow the creation of iterators from a bare pointer.  Perhaps this
// class should be optional, but cache things that iterators may need?
class BitSet {
public:
  static constexpr uint8_t  BUCKET_BITS = 16;
  static constexpr uint32_t BUCKET_SIZE = 1 << BUCKET_BITS; // buckets should be 65536 wide, with all vals sharing top 16 bits
  static constexpr uint32_t BUCKET_SPARSE_MAX = BUCKET_SIZE / 8 / sizeof(uint16_t);   // max cardinality to express as sparse bucket
  static constexpr uint32_t BUCKET_NEG_MIN = BUCKET_SIZE - BUCKET_SIZE / 8; // min cardinality to express as neg set
  using Bits = OpenBitSet<BUCKET_SIZE, uint64_t, uint32_t>; // type for the bits only of a bit bucket

  typedef struct {
    uint16_t upperBits; // upper 16 bits of all values in this bucket
    uint16_t size;      // cardinality of this bucket minus 1
    uint32_t offset;    // location of the data for this bucket
  } BucketDescriptor;


  uint16_t nBuckets;


  typedef struct SPARSE_BUCKET {

  };



  BitSet(void* pointerToEnd) {
    nBuckets = *((uint16_t*)pointerToEnd - 1);
  }


};




// TODO: make a base class / derived class so we can change the flushing mechanism.
class Builder {
public:
  using Bits = BitSet::Bits;
  static constexpr uint32_t SCRATCH_SIZE = 1024;  // perhaps template on this, or just pass it in as a value... only consulted during flushing a bucket anyway.

protected:
  using BucketDescriptor = BitSet::BucketDescriptor;

  // number of bucket descriptors per scratch buffer
  const uint32_t maxDescriptors = (SCRATCH_SIZE - sizeof(char*)) / sizeof(BucketDescriptor);

  uint16_t *values;
  Bits bits;


  uint32_t currBucket = 0;
  uint32_t bucketSize = 0;  // number of values in the current bucket
  uint32_t totalCard = 0;   // total cardinality of the set so far
  int bucketIdx = -1;       // index of the bucket descriptor within the current scratch space

  uint32_t bucketStart = 0;

  // The scratch buffers are only used to store bucket descriptors, not the actual buckets themselves.
  void* startScratch;    // the first scratch buffer
  void* scratch;         // the current scratch buffer
  uint32_t scratchSize;  // size of each scratch buffer
public:
  Builder(void* buf8192_a, void* buf8192_b, void* scratchBuf, uint32_t scratchSize) :
         maxDescriptors((scratchSize - sizeof(char*)) / sizeof(BucketDescriptor)),
         values((uint16_t*)buf8192_a),
         bits((Bits::word_type*)buf8192_b),
         startScratch(scratchBuf),
         scratch(scratchBuf),
         scratchSize(scratchSize) {




  }

  void add(uint32_t val) {
    auto bucket = val >> BitSet::BUCKET_BITS;
    auto lowerBits = (uint16_t)val;
    if (bucket != currBucket) {
      assert(currBucket > bucket);
      flushBucket();
    }
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

  void flushBucket() {
    if (bucketSize == 0) {
      // this can happen if nothing was added for the first bucket.
      return;
    }

    uint32_t writeSize = 0;

    // find space for our bucket descriptor
    if (++bucketIdx >= maxDescriptors) {
      auto oldScratch = scratch;
      scratch = allocateScratch();
      *(char**)((char*)oldScratch + scratchSize - sizeof(char*)) = scratch;  // link old block to new block
      bucketIdx = 0;
    }
    BucketDescriptor& desc = ((BucketDescriptor*)scratch)[bucketIdx];
    desc.upperBits = currBucket;
    desc.size = bucketSize - 1;
    desc.offset = bucketStart;

    if (bucketSize <= BitSet::BUCKET_SPARSE_MAX) {
      // TODO: if (nVals <= 2) {}
      writeSize = bucketSize * sizeof(uint16_t);
      writeBytes((void *) values, writeSize);
    } else {
      // fill in the rest of the bits from the buffered values
      for (uint32_t i=0; i < bucketSize; i++) {
        bits.set(currBucket + values[i]);
      }

      // TODO: store as negative set if too big
      writeSize = Bits::sizeInBytes;
      writeBytes((void *) bits.words, writeSize);
    }

    bucketStart += writeSize;
    bucketSize = 0;
  }

  void flush() {
  }



protected:
  void writeBytes(void* ptr, uint32_t nbytes) {
    out.write(ptr, nbytes);
  }

  // return a new buffer of size Builder::SCRATCH_SIZE, used to keep track of bucket info
  // until flush() is called.
  char* allocateScratch() {
  }
};


}