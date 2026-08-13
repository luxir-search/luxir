#pragma once

#include <memory>
#include <algorithm>
#include <assert.h>
#include <memory_resource>
#include <span>
#include <boost/container/small_vector.hpp>

#include "solux_util.h"
#include "solux/util/log.h"


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
// TODO: support allocating larger blocks of memory than BYTE_BLOCK_SIZE.
//

namespace solux {

// A deleter that only calls the destructor but doesn't delete the memory.
// Non-template so that u_ptr<Derived> and u_ptr<Base> share a deleter type - that lets
// unique_ptr's standard converting assignment work for u_ptr<Derived> -> u_ptr<Base>
// (with the usual requirement that the base has a virtual destructor).
struct no_delete {
  no_delete() = default;
  no_delete(const no_delete&) = default;
  no_delete(no_delete&&) = default;
  no_delete& operator=(const no_delete&) = default;
  no_delete& operator=(no_delete&&) = default;

  template <typename T>
  void operator()(T* ptr) const noexcept { ptr->~T(); }
};

/// solux::u_ptr<T> is a unique_ptr that calls the destructor but doesn't call delete.
/// It can be used to allocate objects from a pool but track the lifetime outside of the pool.
/// The pool should obviously not be rewound or destroyed while there are still u_ptrs to objects.
template <typename T>
using u_ptr = std::unique_ptr<T, no_delete>;

/// Make a unique_ptr to T at a given address.
template <typename T, typename... Args>
u_ptr<T> make_unique_at(void* storage, Args&&... args) {
  T* pointer = new (storage) T(std::forward<Args>(args)...);
  return u_ptr<T>(pointer);
}

/// MemPool implements pmr::memory_resource, but it can also be used directly without virtual dispatch
/// overhead.  Just use getAllocator() for a non-pmr allocator, or use alloc() functions directly.
/// Do not use allocate() directly as is a pmr::memory_resource method that will use virtual methods.
class MemPool final : public std::pmr::memory_resource {
public:

    // implement pmr::memory_resource
    // NOTE: this will mean that allocate() dispatches to a virtual method!
    // if you want to avoid that, use alloc() instead.
private:
  static thread_local std::unique_ptr<MemPool> pool;

  void *do_allocate(size_t __bytes, size_t __alignment) override {
    return alloc(__bytes, __alignment);
  }

  void do_deallocate(void *__p, size_t __bytes, size_t __alignment) override {
      unused(__p, __bytes, __alignment);
  }

  bool do_is_equal(const memory_resource &__other) const noexcept override {
    return this == &__other;
  }

public:
  /// A custom allocator that uses a MemPool. All allocators that use the same pool are
  /// considered equal.
  template <typename T> class allocator {
  public:
    using value_type = T;
    MemPool& pool;
    explicit allocator(MemPool& pool) : pool(pool) {
    }
    // We need this templated to work for classes like std::map
    template <typename U> allocator(const allocator<U>& other) : pool(other.pool) {
    }

    template<typename U> struct rebind { typedef allocator<U> other; };

    T* allocate(std::size_t n) {
      return static_cast<T*>((void*)pool.alloc(n * sizeof(T), alignof(T)));
    }

    void deallocate(T* p, std::size_t n) {
      unused(p,n);
    }

    template <typename U> bool operator==(const allocator<U>& other) const {
      return &pool == &other.pool;
    }
    template <typename U> bool operator!=(const allocator<U>& other) const {
      return !(*this == other);
    }
  };

  allocator<char> getAllocator() {
    return allocator<char>(*this);
  }


#ifndef MEMPOOL_MALLOC
  using save_point = char *;
#else
  using save_point = std::pair<size_t,size_t>;  // pointers.size(), pool.size() pair
#endif
  static constexpr char SCRIBBLE_CHAR = 'Z';

// TODO - make a lot of this stuff private
  static constexpr uint32_t STATIC_BUFFER_SIZE = 256; // number of bytes allocated with this pool before using heap

  // BYTE_BLOCK_SHIFT of 15 gives 32K blocks.  To try and release memory back to the OS (mmap), use a block size of 128K or
  // larger. 18 gives 256K blocks.
  static constexpr uint32_t BYTE_BLOCK_SHIFT = 18;
  static constexpr uint32_t BYTE_BLOCK_SIZE = 1 << BYTE_BLOCK_SHIFT;
  static constexpr uint32_t BYTE_BLOCK_MASK = BYTE_BLOCK_SIZE - 1;
  static constexpr uint32_t HEADER_SIZE = sizeof(int32_t); // size of the header at the beginning of each buffer

  /** index into the buffers array pointing to the current buffer used as the head */

  int bufferIdx = -1;                        // which buffer we are in

  /** Where we are in head buffer */
  int pos;

  /** Current head buffer */
  char *buffer;

  boost::container::small_vector<char*, 4> buffers;

  size_t allocSize = 0;  // sum of all allocations in the pool

  char staticBuffer[STATIC_BUFFER_SIZE];  // static buffer for small pools


  // These place a real T in the pool, so they allocate at alignof(T): the pool
  // hands out bytes at whatever offset the previous allocation ended, and a T
  // whose members want 8-byte alignment sitting on an odd offset is UB (it is
  // what -fsanitize=alignment reports).  The _align variants are only for
  // OVER-aligning past alignof(T), e.g. a SIMD buffer.  Raw byte buffers that
  // are not a T still go through alloc(size) and stay unaligned by design -
  // read those with load/storeUnaligned or a SOLUX_UNALIGNED struct.
  template <typename T, typename... Args>
  u_ptr<T> make_unique(Args&&... args) {
    char* storage = alloc(sizeof(T), alignof(T));
    return make_unique_at<T>(storage, std::forward<Args>(args)...);
  }

  template <typename T, typename... Args>
  u_ptr<T> make_unique_align(size_t alignment, Args&&... args) {
    assert(alignment >= alignof(T));
    char* storage = alloc(sizeof(T), alignment);
    return make_unique_at<T>(storage, std::forward<Args>(args)...);
  }

  template <typename T, typename... Args>
  T* make(Args&&... args) {
    static_assert(std::is_trivially_destructible<T>::value, "type for MemPool::make() must be trivially destructible");
    char* storage = alloc(sizeof(T), alignof(T));
    return new (storage) T(std::forward<Args>(args)...);
  }
  template <typename T, typename... Args>
  T* make_align(size_t alignment, Args&&... args) {
    static_assert(std::is_trivially_destructible<T>::value, "type for MemPool::make() must be trivially destructible");
    assert(alignment >= alignof(T));
    char* storage = alloc(sizeof(T), alignment);
    return new (storage) T(std::forward<Args>(args)...);
  }

  template <typename T, typename... Args>
  std::vector<T, MemPool::allocator<T>>* make_vec(Args&&... args) {
    static_assert(std::is_trivially_destructible<T>::value, "element type for MemPool::make_vec() must be trivially destructible");
    using vec_type = std::vector<T, MemPool::allocator<T>>;
    char* storage = alloc(sizeof(vec_type), alignof(vec_type));
    return new (storage) vec_type(std::forward<Args>(args)..., getAllocator());
  }

  // make an array of default initialized elements
  template <typename T>
  T* make_arr(size_t size) {
    static_assert(std::is_trivially_destructible<T>::value, "element type for MemPool::make_arr() must be trivially destructible");
    char* storage = alloc(sizeof(T)*size, alignof(T));
    return new (storage) T[size]();  // default initialize or uninitialized?
  }

  // make an array of UNINITIALIZED trivial elements: a large capacity
  // reservation touches no memory until the caller writes it, unlike
  // make_arr's value-initialization (which zeroes the whole span).
  template <typename T>
  T* make_arr_uninit(size_t size) {
    static_assert(std::is_trivially_default_constructible<T>::value
                      && std::is_trivially_destructible<T>::value,
                  "element type for MemPool::make_arr_uninit() must be trivial");
    char* storage = alloc(sizeof(T)*size, alignof(T));
    return new (storage) T[size];
  }

  template <typename T>
  std::span<T> make_span(size_t size) {
    T* arr = make_arr<T>(size);
    return {arr, size};
  }

  template <typename T>
  std::span<T> make_span_uninit(size_t size) {
    T* arr = make_arr_uninit<T>(size);
    return {arr, size};
  }

  template <typename T>
  std::span<T> copy_span(std::span<T> span) {
    T* arr = make_arr<T>(span.size());
    std::copy(span.begin(), span.end(), arr);
    return {arr, span.size()};
  }

  // TODO: keep track of high water mark?
  // TODO: keep debugging statistics? (number of pool allocations, size breakdown, wasted space at end of blocks, etc..)

#ifdef MEMPOOL_MALLOC
  // pointers[bbAddr] -> heap pointer
  std::vector< std::unique_ptr<char[]> > pointers;
  size_t allocated = 0;
#endif

  // TODO: accept an upstream allocator / memory resource

  MemPool(const MemPool &) = delete;

  MemPool &operator=(const MemPool &) = delete;

  MemPool() {
    initNewBuffer(staticBuffer, STATIC_BUFFER_SIZE);
  }

  ~MemPool();

  void initNewBuffer(char* newBuffer, uint32_t size) {
    buffers.push_back(newBuffer);
    bufferIdx++;
    assert((size_t)bufferIdx == buffers.size() - 1);
    storeUnaligned<uint32_t>(newBuffer, size);
    buffer = newBuffer;
    pos = HEADER_SIZE;  // start after the header
    allocSize += size;
  }

  size_t allocatedSize() const {
    return allocSize;
  }

  uint32_t bufferSize(const char* buf) const {
    return loadUnaligned<uint32_t>(buf);
  }

  // TODO: avoid using ptr() directly since it won't work when switching to malloc
  char *ptr() { return buffer + pos; }

  // Byte-block addresses are UNSIGNED 32-bit (4 GiB pool ceiling by design);
  // signed handling here used to wrap at 2 GiB and index buffers[] negatively.
  char *ptr(uint32_t bbAddr) const {
#ifndef MEMPOOL_MALLOC
    return buffers[bbAddr >> BYTE_BLOCK_SHIFT] + (bbAddr & BYTE_BLOCK_MASK);
#else
    return pointers[bbAddr].get();
#endif
  }

  uint32_t bbAddress() {
#ifndef MEMPOOL_MALLOC
    // bufferIdx must widen BEFORE the shift: as int it overflows at 2 GiB
    return ((uint32_t) bufferIdx << BYTE_BLOCK_SHIFT) | (uint32_t) pos;
#else
    return (int)pointers.size();
#endif
  }

  size_t size() {
#ifndef MEMPOOL_MALLOC
    return allocSize - bufferSize(buffer) + pos;
#else
    return allocated;
#endif
  }

  size_t capacity() {
#ifndef MEMPOOL_MALLOC
    return allocSize;
#else
    return size();
#endif
  }

  /// space left in the current buffer
  size_t spaceLeft() {
    return bufferSize(buffer) - pos;
  }

  /// Returns pointer to start of data in the current buffer (past the header).
  char* blockStart() {
    return buffer + HEADER_SIZE;
  }

  /// Shrink the last allocation by n bytes (backs up pos).
  void shrink(int n) {
    assert((int)pos - n >= (int)HEADER_SIZE);
    pos -= n;
  }

  char* reserve(uint32_t size) {
    assert(size <= BYTE_BLOCK_SIZE - HEADER_SIZE);
    if (spaceLeft() >= size) {
      return ptr();
    }
    // not enough space in the current buffer, allocate a new one
    backupAlloc(size);
    pos -= size;
    assert(pos == HEADER_SIZE);  // pos should be after the header after allocation
    return ptr();
  }

  // ensures that there is enough space starting with the current buffer (moving to a new buffer if necessary)
  // and returns the resulting end offset into the last (current) block.
  int reserveBBP(uint32_t size) {
    assert(size <= BYTE_BLOCK_SIZE - HEADER_SIZE);
    auto newEnd = pos + size;
    if (newEnd > bufferSize(buffer)) {
      nextBuffer(size);
      newEnd = size + HEADER_SIZE;  // reserve space for the header
    }
    return newEnd;
  }

  uint32_t allocateBBP(uint32_t size) {
#ifndef MEMPOOL_MALLOC
    int newEnd = reserveBBP(size);
    // char* p = buffer + pos;
    uint32_t bbAddr = bbAddress();
    pos = newEnd;
    return bbAddr;
#else
    pointers.emplace_back( new char[size] );
    allocated += size;
    // if (pointers.size() < 100) { std::cout << "PTR=" << (void*)(pointers.back().get()) << "\tnum="  << (pointers.size()-1) << std::endl; }
    // we could scribble over memory, but that would confuse other memory checkers
    return (uint32_t)pointers.size() - 1;
#endif
  }


  char *alloc(size_t size) {
#ifndef MEMPOOL_MALLOC

    size_t newPos = pos + size;
    if (newPos <= bufferSize(buffer)) {
      // there is enough space in the current buffer
      char* p = buffer + pos;
      pos = (int)newPos;
      return p;
    } else {
      return backupAlloc(size);
    }

    /*
    int newEnd = reserveBBP(size);
    auto p = ptr();
    pos = newEnd;
    return p;
    */
#else
    allocateBBP(size);
    return pointers.back().get();
#endif
  }

  char *alloc(size_t size, size_t alignment) {
#ifndef MEMPOOL_MALLOC
    align(alignment);
    size_t newPos = pos + size;
    if (newPos <= bufferSize(buffer)) {
      // there is enough space in the current buffer
      char* p = buffer + pos;
      pos = (int)newPos;
      return p;
    } else {
      return backupAlloc(size, alignment);
    }
#else
    allocateBBP(size);
    return pointers.back().get();
#endif
  }

  // do allocation and return both the normal pointer as well as the short pool specific pointer (bbptr)
  std::pair<char *, uint32_t> allocateAddrs(uint32_t size) {
#ifndef MEMPOOL_MALLOC
    int newEnd = reserveBBP(size);
    uint32_t bbAddr = bbAddress();
    auto p = ptr();
    pos = newEnd;
    return {p, bbAddr};
#else
    auto bbAddr = allocateBBP(size);
    auto p = pointers.back().get();
    return {p, bbAddr};
#endif
  };

  void align(size_t alignment = 8) {
#ifndef MEMPOOL_MALLOC
    // Do the alignment on the pointer and not the position so that we can handle larger alignments than the default
    // this can bring us past the end of the block, but that's ok
    auto p = ptr();
    char* newPtr = (char*)(((size_t)p + alignment - 1) & ~(alignment - 1));
    pos = newPtr - buffer;
#else
#endif
  }

  template<class T>
  uint32_t allocateTypeAligned(T *&out) {
#ifndef MEMPOOL_MALLOC
    align();
    int newEnd = reserveBBP((int) sizeof(T));
    out = reinterpret_cast<T *>( ptr());
    uint32_t bbAddr = bbAddress();
    pos = newEnd;
    return bbAddr;
#else
    return allocateBBP(sizeof(T));
#endif
  }

  bool scribble(char *p, size_t len) {
    memset(p, SCRIBBLE_CHAR, len);
    return true;
  }

  void _rewind(const save_point &savePoint, uint32_t buffersToSave);

  /// Rewinds to the rewindPoint, effectively deallocating all allocations after that point.
  /// buffersToSave is the number of buffers to hold in reserve for use during subsequent pool expansions.
  /// If you are going to be repeating some type of work you just did, and hence expect the same order of
  /// magnitude of memory allocation, consider passing INT_MAX.  Otherwise, 1 may be a good general purpose choice.
  /// 0 may be better if one has many pools.
  void rewind(const save_point &savePoint, uint32_t buffersToSave = 1) {
#ifndef MEMPOOL_MALLOC
    if (savePoint >= buffer && savePoint <= buffer + bufferSize(buffer)) {
      // fast path: same buffer
      assert(scribble(savePoint, ptr() - savePoint));  // scribble from the save point to the current point
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
    MemPool &pool_;
    save_point savePoint;
  public:
    explicit ScopeGuard(MemPool &pool, const save_point &savePoint) : pool_(pool), savePoint(savePoint) {}

    explicit ScopeGuard(MemPool &pool) : pool_(pool), savePoint(pool.getSavePoint()) {}

    MemPool& pool() {
      return pool_;
    }

    ~ScopeGuard() {
      pool_.rewind(savePoint);
    }

    ScopeGuard(const ScopeGuard &) = delete;

    ScopeGuard(ScopeGuard &&) = delete;

    ScopeGuard &operator=(const ScopeGuard &) = delete;

    ScopeGuard &operator=(ScopeGuard &&) = delete;
  };

  ScopeGuard rewindScopeGuard() {
    return ScopeGuard(*this);
  }


  static MemPool& threadLocal() {
    if (!pool) {
      pool = std::make_unique<MemPool>();
    }
    return *pool;
  }

  /// Convenience method so you don't forget to rewind the pool when it goes out of scope.
  /// Call .pool() on the returned ScopeGuard to get the pool.
  static ScopeGuard threadLocalPoolGuard() {
    return ScopeGuard(threadLocal());
  }

  // Checks that any thread-local pools are empty.
  static bool sanityCheck() {
    if (pool && pool->size() > MemPool::HEADER_SIZE) {
      LOG_ERROR("Thread-local pool not empty! size={}", pool->size());
      pool.reset();  // just delete the pool so we don't leak memory
      return false;
    }
    return true;
  }

  void nextBuffer(size_t size);
  char* backupAlloc(size_t size);
  char* backupAlloc(size_t size, size_t alignment);
};


} // end namespace