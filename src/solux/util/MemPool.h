#pragma once

#include <memory>
#include <iostream>
#include <string.h>
#include <vector>
#include <tuple>
#include <algorithm>
#include <assert.h>
#include <unordered_set>

#include "solux_util.h"


// #define MEMPOOL_MALLOC true   // use malloc/new for each individual allocation for better memory checking with checkers

// TODO: allocate in large enough chunks so that we use MMAP so it can be released back to OS?
// Could start allocating pages in groups of 4 after some limit (128K is default to go to mmap in glibc at least)
// TODO: separate 16 byte aligned pool
// Streams could be allocated in a pool with 16 byte alignment (all block pointers would be aligned, thus
// We could use the 16 byte aligned pool for new fields
// 1 byte aligned pool would be for new terms
// the streams of those terms would be in the 16 pool
// If the mem usage is unique terms, we still run out after 2GB in the 1 byte pool?
//   NO!!! because we have 64 bit pointers and only care about 4 byte addresses for forward pointers in streams (which always point to 16 byte pool)
// We also don't have to keep the list of 1-align pages in a vector (contiguous) since we won't be looking up by bbAddr
// although resizing that prob wouldn't be noticable w/ everything else that needs to go on to fill a page.
// Actually, the 1-align pages (if not bbAddr addressable) can be variable sized!!!!!!!!!!!!!!! (standard memory pool?)
// Possible downside: worse cache locality? Actually, cache locality should be bad anyway, even with a single pool.  The stream parts are not contiguous
//
// TODO: when we get longer lists of deltas, we could try and compress them better with some other method?
// Then use the added space for more vints at the end?
//
// Real space killers can be unique terms though... esp unique IDs.
// Any way to do prefix compression or something?
// TODO: make sure we're actually using the first page of memory!
//
// TODO: try out code that avoids crossing a page (4K) boundary (for struct allocations)
//
// TODO: instead of 4 byte forward pointers, we can subtract the two pointers (or rather subtract the end of this stream block
// with the start of the next stream block.  Take the difference and delta code that in reverse from the end of the block.
// If we are using 64 byte aligned blocks, then we can right shift the difference to save even more space.  Store a sign bit or
// zig-zag encode to deal with negatives.  Look at gRPC encoding/decoding to see if there are any tricks to speed / branch prediction.
// Once we no longer need bbAddr addresses, we can rapidly increase the size of the allocated buffers, decreasing the amount of time
// spent in allocations, and making it easier for the kernel to use large pages?
//
// TODO: ability to grab a pointer to the end of the pool, use the pool for other uses, then
// rewind to that end point.  Useful for quickly adding a few docs to a random index.  Can compare
// these pointers to get how much memory was used for an operation as well.
//

class MemPool {
public:
#ifndef MEMPOOL_MALLOC
  using save_point = char *;
#else
  using save_point = std::pair<size_t,size_t>;  // pointers.size(), pool.size() pair
#endif
  static constexpr char SCRIBBLE_CHAR = 'Z';

// TODO - make a lot of this stuff private
  static constexpr int BYTE_BLOCK_SHIFT = 15;
  static constexpr int BYTE_BLOCK_SIZE = 1 << BYTE_BLOCK_SHIFT;
  static constexpr int BYTE_BLOCK_MASK = BYTE_BLOCK_SIZE - 1;

  // todo - try unique_ptr here and see if it slows anything down
  std::vector<char *> buffers;

  /** index into the buffers array pointing to the current buffer used as the head */

  int bufferIdx = -1;                        // which buffer we are in

  /** Where we are in head buffer */
  int pos = BYTE_BLOCK_SIZE;

  /** Current head buffer */
  char *buffer = nullptr;


  // TODO: keep track of high water mark?
  // TODO: keep debugging statistics? (number of pool allocations, size breakdown, wasted space at end of blocks, etc..)

#ifdef MEMPOOL_MALLOC
  // pointers[bbAddr] -> heap pointer
  std::vector< std::unique_ptr<char[]> > pointers;
  size_t allocated = 0;
#endif

  // TODO: accept an upstream allocator / memory resource

  MemPool(const MemPool &) = delete;
  MemPool& operator=(const MemPool&) = delete;

  MemPool();
  ~MemPool();


  // TODO: avoid using ptr() directly since it won't work when switching to malloc
  char* ptr() { return buffer + pos; }

  char* ptr(int bbAddr) const {
#ifndef MEMPOOL_MALLOC
    return buffers[bbAddr >> BYTE_BLOCK_SHIFT] + (bbAddr & BYTE_BLOCK_MASK);
#else
    return pointers[bbAddr].get();
#endif
  }

  int bbAddress() {
#ifndef MEMPOOL_MALLOC
    return (bufferIdx << BYTE_BLOCK_SHIFT) | pos;
#else
    return (int)pointers.size();
#endif
  }

  int size() {
#ifndef MEMPOOL_MALLOC
    return bufferIdx * BYTE_BLOCK_SIZE + pos;
#else
    return allocated;
#endif
  }

  int capacity() {
#ifndef MEMPOOL_MALLOC
    return buffers.size() * BYTE_BLOCK_SIZE;
#else
    return size();
#endif
  }

  // ensures that there is enough space starting with the current buffer (moving to a new buffer if necessary)
  // and returns the resulting end offset into the last (current) block.
  int reserveBBP(uint32_t size) {
    assert(size <= BYTE_BLOCK_SIZE);
    auto newEnd = pos + size;
    if (newEnd > BYTE_BLOCK_SIZE) {
      nextBuffer();
      newEnd = size;
    }
    return newEnd;
  }

  int allocateBBP(uint32_t size) {
#ifndef MEMPOOL_MALLOC
    int newEnd = reserveBBP(size);
    // char* p = buffer + pos;
    int bbAddr = bbAddress();
    pos = newEnd;
    return bbAddr;
#else
    pointers.emplace_back( new char[size] );
    allocated += size;
    // if (pointers.size() < 100) { std::cout << "PTR=" << (void*)(pointers.back().get()) << "\tnum="  << (pointers.size()-1) << std::endl; }
    // we could scribble over memory, but that would confuse other memory checkers
    return (int)pointers.size() - 1;
#endif
  }


  char* allocate(size_t size) {
#ifndef MEMPOOL_MALLOC
    int newEnd = reserveBBP(size);
    auto p = ptr();
    pos = newEnd;
    return p;
#else
    allocateBBP(size);
    return pointers.back().get();
#endif
  }

    // do allocation and return both the normal pointer as well as the short pool specific pointer (bbptr)
    std::pair<char *, int> allocateAddrs(uint32_t size) {
#ifndef MEMPOOL_MALLOC
    int newEnd = reserveBBP(size);
    int bbAddr = bbAddress();
    auto p = ptr();
      pos = newEnd;
    return {p, bbAddr};
#else
    auto bbAddr = allocateBBP(size);
    auto p = pointers.back().get();
    return {p, bbAddr};
#endif
  };

    void align() {
#ifndef MEMPOOL_MALLOC
        unsigned align = 8;
      pos = (pos + (align - 1)) & -align;
#else
#endif
    }

  template<class T>
  int allocateTypeAligned(T *&out) {
#ifndef MEMPOOL_MALLOC
    align();
    int newEnd = reserveBBP((int) sizeof(T));
    out = reinterpret_cast<T *>( ptr());
    int bbAddr = bbAddress();
    pos = newEnd;
    return bbAddr;
#else
    return allocateBBP(sizeof(T));
#endif
  }

  bool scribble(char* p, size_t len) {
    memset(p, SCRIBBLE_CHAR, len);
    return true;
  }

  void _rewind(const save_point& savePoint, uint32_t buffersToSave);

  /// Rewinds to the rewindPoint, effectively deallocating all allocations after that point.
  /// buffersToSave is the number of buffers to hold in reserve for use during subsequent pool expansions.
  /// If you are going to be repeating some type of work you just did, and hence expect the same order of
  /// magnitude of memory allocation, consider passing INT_MAX.  Otherwise, 1 may be a good general purpose choice.
  /// 0 may be better if one has many pools.
  void rewind(const save_point& savePoint, uint32_t buffersToSave=1) {
#ifndef MEMPOOL_MALLOC
    if (savePoint >= buffer && savePoint <= buffer + BYTE_BLOCK_SIZE) {  // TODO: check boundary condition here...
      // fast path: same buffer
      assert(scribble(savePoint, ptr()-savePoint));  // scribble from the save point to the current point
      // if sp==buffer+pos, then pos=sp-buffer to restore.
      pos = savePoint - buffer;
    } else {
      _rewind(savePoint, buffersToSave);
    }
#else
    _rewind(savePoint, buffersToSave);
#endif
  }

  // returns enough information for a call to rewind() to deallocate all allocations after the save point
  save_point getSavePoint() {
#ifndef MEMPOOL_MALLOC
    return ptr();
#else
    return {pointers.size(), this->size()};
#endif
  }

  class ScopeGuard {
    MemPool& pool;
    save_point savePoint;
  public:
    explicit ScopeGuard(MemPool& pool, const save_point& savePoint) : pool(pool), savePoint(savePoint){}
    explicit ScopeGuard(MemPool& pool) : pool(pool), savePoint(pool.getSavePoint()){}
    ~ScopeGuard() {
      pool.rewind(savePoint);
    }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;
  };

  ScopeGuard rewindScopeGuard() {
    return ScopeGuard(*this);
  }


  void nextBuffer();
};


