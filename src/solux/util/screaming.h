#pragma once

#include <cstdint>
#include <vector>
#include <bit>
#include <cstring>
#include <memory_resource>
#include <sstream>
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
// This can also be used as the basis for dynamically sized bitsets, just
// don't use methods that rely on size.
template <uint64_t size_, typename word_type_, typename index_type_>
class OpenBitSet {
public:
  using word_type = word_type_;
  using index_type = index_type_;
  static constexpr uint64_t size = size_;
  static constexpr index_type sizeInBytes = size / 8;
  static constexpr index_type numWords = sizeInBytes / sizeof(word_type);
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
    assert(val < size);
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    words[wordIdx] |= (word_type)1 << bitIdx;
  }

  bool get(index_type val) const {
    assert(val < size);
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    return words[wordIdx] & ((word_type)1 << bitIdx);
  }

  // returns 0 or 1
  int getInt(index_type val) const {
    assert(val < size);
    index_type wordIdx = val >> wordShift;
    uint8_t bitIdx = val & wordMask;
    return (words[wordIdx] >> bitIdx) & 0x01;
  }


  // Returns the next set bit or MAX_INDEX if none exists.
  index_type nextSetBit(index_type val) const {
    assert(val < size);
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

  void flip() {
    for (index_type i = 0; i<numWords; i++) {
      words[i] = ~words[i];
    }
  }

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
  static constexpr uint32_t RANK_INDEX_SIZE = (Bits::numWords >> RANK_INDEX_SHIFT) * sizeof(uint16_t);
  static constexpr uint32_t SPARSE_CONTAINER_SIZE = Bits::sizeInBytes;
  static constexpr uint32_t DENSE_CONTAINER_SIZE = Bits::sizeInBytes + RANK_INDEX_SIZE;


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
      if (next < bucketBase + Bits::size) {
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

    int32_t rank() {
      // If we always want rank on every advance, we could do this more efficiently by updating the rank
      // in the advance method.

      switch (bucketType) {
        case SPARSE:
          return bucketRank + bucket.sparse.index;
        case DENSE:
          auto localIndex = uint16_t(curr);
          int rankIdx = localIndex >> (Bits::wordShift + RANK_INDEX_SHIFT);
          uint16_t *rankIndexArr = reinterpret_cast<uint16_t *>(bucket.bits.obs.words + Bits::numWords);
          auto rankBase = rankIndexArr[rankIdx];
          // Now find the rank of words before the current word in our mini-block
          int wordIndex = localIndex >> Bits::wordShift;
          constexpr uint16_t miniBlockMask = (1 << RANK_INDEX_SHIFT) - 1;
          for (int i = wordIndex & ~miniBlockMask; i < wordIndex; i++) {
            rankBase += std::popcount(bucket.bits.obs.words[i]);
          }
          uint8_t bitIdx = localIndex & Bits::wordMask;
          auto bitsToTheRight = bitIdx == 0 ? 0 : (bucket.bits.obs.words[wordIndex] << (sizeof(Bits::word_type)*8 - bitIdx));
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



  };




protected:

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

  uint32_t cardinality() {
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
        uint16_t* rankIndexArr = reinterpret_cast<uint16_t*>(bits.words + bits.numWords);
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
        for (int i=0; i<bits.numWords - wordsPerCount; i += wordsPerCount) {
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
  friend class Builder;
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