#pragma once

#include <cstdint>
#include <iostream>
#include "solux/util/solux_util.h"
#include "solux/util/MemPool.h"

namespace solux {

class StreamReader;

class END {
};

// TODO: try using a 64 byte aligned stream! (see comments at start of DocStream.h)
//   we wouldn't need to store how much left (calc it), size of the current slice (always 64)
//   we would really only need to store the current location and bbStart!  I don't think there
//   is any reason to have to know the number of allocated bytes.  If we only want to support
//   4GB, the current pointer could be an offset (BB address) instead of a 64 bit pointer.
//   Reminder to align the malloc'd blocks to 64B as well.

// Hmmm, when there are 3 streams together, it would be better coded as
// ptr,ptr,ptr,len,len,len to avoid extra padding!  termdoc, termpos
SOLUX_PACKED_START
class Stream {
public:

  typedef StreamReader iterator; // TODO: should this be const_iterator, forward_iterator?

  static constexpr uint8_t nextSliceSize(uint8_t currentSliceSize) {
    unsigned curr = currentSliceSize;
    curr += (currentSliceSize >> 2) + 6;  // Starting at 4, we get 11 19 29 42 58 78 103 134 173 222 283
    // if it ever mattered, we could probably find a way to replace the std::min with a mask
    // TODO: what about a version that returns round numbers for better mixing with other structures?
    return (uint8_t) std::min(curr, 200u);
  }

  /**
   * The first level size for new stream slices
   */
  static constexpr int FIRST_LEVEL_SIZE = sizeof(int);

  char *ptr_;    // pointer to the last byte of the stream

  // ptr_ starts out pointing at bbStart_, which can hold 4 bytes.  Then when we write the forwarding address, it will be equal to
  // what we would have set bbStart_ to anyway!  When reading back, we just need to check size.
  // TODO: convert to union?
  int bbStart_;  // byte block address of the start of the stream. For 2 related streams (freq, prox), the second can just be an offset from the first... (but not if we use the optimization where we store data here first!)
  int allocatedSz_; // total number of bytes allocated in the stream (i.e. subtract amount left over to get size of stream).  Does not include pointer bytes.

  uint8_t left_;  // size left in the slice     // TODO : pack left_ or slizeSz_ in ptr_?
  uint8_t sliceSz_; // size of the current slice

private:
  // Stream(Stream&& o);

public:

  Stream() {
    // this creates an alias, but we don't actually *do* anything with bbStart_ when writing
    // Ah... but we do support move operators... if those were used / optimized, it could mean big troubles.
    ptr_ = reinterpret_cast<char *>(&bbStart_);
    // bbStart_ member is uninitialized, and that's OK
    sliceSz_ = sizeof(int);
    left_ = sliceSz_;
    allocatedSz_ = sliceSz_;
    // std::cout << "CONSTRUCT!" << std::endl;
  }

  // TODO: we could also just prohibit moves...
  void moveFrom(Stream &o) {
    std::cout << "MOVED!" << std::endl;  // nocommit
    ptr_ = o.allocatedSz_ == sizeof(int) ? (reinterpret_cast<char *>(&bbStart_) + (sizeof(int) - o.left_)) : o.ptr_;
    bbStart_ = o.bbStart_;
    allocatedSz_ = o.allocatedSz_;
    left_ = o.left_;
    sliceSz_ = o.sliceSz_;
  }

  Stream(const Stream &o) = delete;

  void operator=(const Stream &o) = delete;

  Stream(Stream &&o) {
    moveFrom(o);
  }

  Stream &operator=(Stream &&o) {
    moveFrom(o);
    return *this;
  }

  const char *ptr(const MemPool &pool) const {
    unused(pool);
    return ptr_;
  }

  int size(const MemPool &pool) const {
    unused(pool);
    return allocatedSz_ - left_;
  }

  int left(const MemPool &pool) const {
    unused(pool);
    return left_;
  }

  const char *start(const MemPool &pool) const {
    unused(pool);
    if (allocatedSz_ == FIRST_LEVEL_SIZE) {
      return reinterpret_cast<const char *>(&bbStart_);
    } else {
      return pool.ptr(bbStart_);
    }
  }

  /** Pointer to previously written byte.  Only valid if a byte as been previously written. */
  char *prevPtr(MemPool &pool) {
    unused(pool);
    assert(size(pool) > 0);
    return ptr_ - 1;
  }

  void writeByte(MemPool &pool, uint8_t val) {
    if (left_ == 0) {
      // pull this out into a function?
      sliceSz_ = nextSliceSize(sliceSz_);
      int blockAddr = pool.allocateBBP(sliceSz_);

      char *newPointer = pool.ptr(blockAddr);  // TODO: what about a version that returns pointer and the block address
      // move last 4 bytes to new area... we do this in one chunk using an integer.
      // this works for both both little endian and big endian since we're only moving.
      int *lastWord = reinterpret_cast<int *>(ptr_ - 4);
      *reinterpret_cast<int *>(newPointer) = *lastWord;
      *lastWord = blockAddr;  // point to the new area with the last 4 bytes of the old area
      // TODO: this can alias bbStart_... is there anything we can do before reading bbStart_ to ensure this write is seen?
      // NOTE: we never read bbStart_ during the inversion phase of indexing, only when flushing.

      ptr_ = newPointer + 4;
      allocatedSz_ += sliceSz_ - 4;
      left_ = static_cast<uint8_t>( sliceSz_ - 4 );
    }

    *ptr_ = val;
    ptr_++;
    left_--;
  }

  // TODO: if we write doc freqs in big endian, we don't need to keep track of lastDocCode
  // since we can just directly set bit for a 1 freq on the last octet written.

  // TODO: an optimized version for when we have >= 5 bytes available?
  // TODO: convert to unsigned and do checks elsewhere?
  void writeVInt(MemPool &pool, int val) {
    assert(val >= 0);
    auto v = (unsigned) val;
    while ((v & ~0x7F) != 0) {
      writeByte(pool, (uint8_t) (v | 0x80));
      v >>= 7;
    }
    writeByte(pool, (uint8_t) v);
  }

  // This version may be be slower... but it might help inlining with only a single call to writeByte (need to test)
  void writeVInt2(MemPool &pool, int val) {
    assert(val >= 0);
    auto v = (unsigned) val;
    for (;;) {
      auto flag = (v & 0x7f) == 0 ? 0 : 0x80;
      writeByte(pool, (uint8_t) (v | flag));
      if (flag == 0) break;
      v >>= 7;
    }
  }


  // TODO: some sort of adapter that will specify the pool for us?
  [[nodiscard]] StreamReader begin(const MemPool &pool) const;

  // const StreamReader& end(const MemPool& pool) const;
  END end(const MemPool &pool) const {
    unused(pool);
    return END();
  }

}
  SOLUX_PACKED_END;


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


// TODO: Stream could also be cast as an output iterator??
class StreamReader {
  const char *ptr_; // if ptr_ is null, all other members may be invalid!
  const MemPool &pool_;
  int remaining_;   // number of data bytes left to consume in the full stream
  uint8_t remainingInSlice_;
  uint8_t sliceSize_;

  void initFromBBPointer(int bbptr) {
    ptr_ = pool_.ptr(bbptr);
    sliceSize_ = Stream::nextSliceSize(sliceSize_);
    remainingInSlice_ = static_cast<uint8_t>( remaining_ > sliceSize_ ? (sliceSize_ - 4) : remaining_ );
  }

  friend class Stream;

public:
  StreamReader(const Stream &source, const MemPool &pool) : pool_(pool) {
    remaining_ = source.size(pool_);

    // position ptr on first character, or null if none
    if (remaining_ <= Stream::FIRST_LEVEL_SIZE) {
      if (remaining_ == 0) {
        ptr_ = nullptr;
      } else {
        ptr_ = reinterpret_cast<const char *>(&source.bbStart_);
        sliceSize_ = Stream::FIRST_LEVEL_SIZE;
        remainingInSlice_ = static_cast<uint8_t>(remaining_);
      }
    } else {
      sliceSize_ = Stream::FIRST_LEVEL_SIZE;
      int bbStart = source.bbStart_;
      // nocommit memcpy(&bbStart, &source.bbStart_, sizeof(int));  // This is just trying to tell the compiler that bbStart_ was written through an alias, so get the actual bytes!
      initFromBBPointer(bbStart);
    }
  }

  void incrementPointer() {
    ++ptr_;

    if (--remainingInSlice_ == 0) {
      if (remaining_ <= sliceSize_) {
        ptr_ = nullptr;
      } else {
        // follow the next link in the chain
        remaining_ -= (sliceSize_ - 4);
        int bbptr = *reinterpret_cast<const int *>(ptr_);
        initFromBBPointer(bbptr);
      }
    }
  }

  StreamReader& operator++() {
    incrementPointer();
    return *this;
  }

  const StreamReader operator++(int) { // postfix ++
    StreamReader prev = *this;
    incrementPointer();
    return prev;
  }

  char operator*() const {
    return *ptr_;
  }


  bool operator==(const END &other) const {
    unused(other);
    return this->ptr_ == nullptr;
  }

  bool operator!=(const END &other) const {
    unused(other);
    return this->ptr_ != nullptr;
  }

  bool eof() const { return ptr_ == nullptr; }

  char readByte() {
    auto ret = *ptr_;
    incrementPointer();
    return ret;
  }
  char read() {
    auto ret = *ptr_;
    incrementPointer();
    return ret;
  }

  // TODO: consolidate with InputStream somehow... templatize?
  uint32_t readVint() {
    char b = readByte();
    uint32_t val = b & 0x7f;
    // TODO: try replacing with a loop to 4 (to avoid running long if data is bad)
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = readByte();
      val |= (b & 0x7f) << shift;
    }
    return val;
  }

};


inline StreamReader Stream::begin(const MemPool &pool) const {
  return StreamReader(*this, pool);
}

} // end namespace
