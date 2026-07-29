#pragma once

#include <stddef.h>
#include <stdint.h>
#include <iostream>
#include <vector>
#include <memory.h>

// NOTE: this is better than including xxhash.h since it enables inline. Inverter performance equal to
// fvn1a when inlined.  25% slower if not inlined.
#define XXH_INLINE_ALL
#define XXH_PRIVATE_API
#include <xxh3.h>


namespace solux {

// This scope guard will call the function when it goes out of scope.
// lvalues are not copied/moved, rvalues are moved.
// Example:
//    auto cleaner = solux::scope_guard([](){ solux::Signal::unlisten("mergeStart");});  // lambda is moved
//    auto cb = [](){ solux::Signal::unlisten("mergeStart");
//    auto cleaner2 = solux::scope_guard(cb);  // lambda is not moved, only referenced.
// Make sure the callback doesn't throw!
template<typename F>
class scope_guard {
  public:
    F func;
  scope_guard(F&& f): func(std::forward<F>(f)) {}
    ~scope_guard() { func();  }
};
template<typename F> scope_guard(F&& frv) -> scope_guard<F>;



// Declares a struct with no internal padding whose objects may sit at any
// address: mapped-file bytes at an offset we do not control, an entry carved
// out of a MemPool, or an element of an array of the struct itself (packing
// away the tail padding is exactly what puts the next element off-alignment).
//
// This tells the compiler alignof == 1, so it emits member accesses that are
// safe for an unaligned object; plain field access then compiles to the same
// instructions a hand-written memcpy would, without materializing the struct.
// On gcc `packed` alone already implies alignof 1 and `aligned(1)` is
// redundant; it is spelled out because the alignment, not the layout, is the
// property callers depend on for correctness.  Layout is unchanged as long as
// members are already padded to their natural offsets (assert the size), so
// aligning these on disk later is a writer-side decision that needs no reader
// change.
//
// use SOLUX_UNALIGNED_START struct X{} SOLUX_UNALIGNED_END;
// gcc and msvc have different ways of spelling this :-(
#ifdef __GNUC__
#define SOLUX_UNALIGNED_START ;
#define SOLUX_UNALIGNED_END __attribute__((__packed__, __aligned__(1)))
#endif

#ifdef _MSC_VER
#define SOLUX_UNALIGNED_START __pragma(pack(push,1))
#define SOLUX_UNALIGNED_END __pragma(pack(pop))
#endif

#if defined(__GNUC__)
#  define SOLUX_INLINE __attribute__((always_inline))
#  define SOLUX_NOINLINE __attribute__((noinline))
#  define SOLUX_RESTRICT __restrict
#elif defined(_MSC_VER)
#  define SOLUX_INLINE __forceinline
#  define SOLUX_NOINLINE __declspec(noinline)
#  define SOLUX_RESTRICT __restrict
#else
#  define SOLUX_INLINE
#  define SOLUX_NOINLINE
#  define SOLUX_RESTRICT
#endif

// Load/store a scalar through bytes that may be unaligned - mapped file data at
// an offset we do not control, or a MemPool entry that starts wherever the
// previous entry ended.  A plain `*(T*)p` on such bytes is UB even on x86:
// nothing faults, but the compiler is entitled to assume alignof(T) and to
// vectorize accordingly, and -fsanitize=alignment flags it.
//
// The obvious `T v; memcpy(&v, p, sizeof v); return v;` compiles to the same
// single load once optimized, but it needs a local, and the local is not free
// in the builds we run tests in: at -O0 `-ftrivial-auto-var-init=pattern`
// stores a fill pattern into it before the copy overwrites it, and ASan gives
// it a shadow-checked fake stack slot (__asan_stack_malloc). Reading through a
// 1-aligned type has no local at all, so every build gets the plain load.
// always_inline, not just inline: at -O0 nothing is inlined by default, and a
// call here would cost more than the local it removes.
#if defined(__GNUC__)
template <class T>
[[gnu::always_inline]] inline T loadUnaligned(const void* p) {
  typedef T __attribute__((__aligned__(1))) unaligned_t;
  return *reinterpret_cast<const unaligned_t*>(p);
}
template <class T>
[[gnu::always_inline]] inline void storeUnaligned(void* p, T value) {
  typedef T __attribute__((__aligned__(1))) unaligned_t;
  *reinterpret_cast<unaligned_t*>(p) = value;
}
#else
template <class T>
SOLUX_INLINE inline T loadUnaligned(const void* p) {
  T value;
  memcpy(&value, p, sizeof(value));
  return value;
}
template <class T>
SOLUX_INLINE inline void storeUnaligned(void* p, T value) {
  memcpy(p, &value, sizeof(value));
}
#endif

template<typename... Args>
inline void unused(Args &&...) {}

// generic vector ostream
template<typename T>
std::ostream &operator<<(std::ostream &out, const std::vector<T> &v) {
  out << '[';
  for (size_t i = 0; i < v.size(); i++) {
    if (i != 0) out << ", ";
    out << v[i];
  }
  out << ']';

  /*** alternative
  out << '[';
  std::copy (v.begin(), v.end(), std::ostream_iterator<T>(out, ", "));
  out << ']';
  ***/

  return out;
}


class Hash {
private:
  static constexpr uint32_t FVN_Prime = 0x01000193; //   16777619
  static constexpr uint32_t FVN_Seed = 0x811C9DC5; // 2166136261

public:
  /// fnv1a hash, descent for hash tables with short keys.
  // We could do better if we knew it was safe to read up to 8 bytes before/after the data...
  // see wyhash (safety=0) and fvn1a-pippin
  static inline uint32_t fvn1a(const void *data, size_t numBytes, uint32_t hash = FVN_Seed) {
    auto ptr = (const unsigned char *) data;
    while (numBytes--)
      hash = (*ptr++ ^ hash) * FVN_Prime;
    return hash;
  }


//-----------------------------------------------------------------------------
// MurMurHash2 (MurmurHash64A), by Austin Appleby, originally in the public domain.
// https://github.com/aappleby/smhasher/blob/master/src/MurmurHash2.cpp
// This version has just been slightly modified to get rid of compiler / IDE warnings & suggestions.
// Does unaligned loads and produces different values on little/big endian.
  static constexpr uint64_t MurmurHash64A(const void * key, size_t len, uint64_t seed)
  {
    const uint64_t m = 0xc6a4a7935bd1e995;
    const int r = 47;

    uint64_t h = seed ^ (len * m);

    auto data = (const uint64_t *)key;
    auto end = data + (len/8);

    while(data != end)
    {
      uint64_t k = *data++;

      k *= m;
      k ^= k >> r;
      k *= m;

      h ^= k;
      h *= m;
    }

    auto data2 = (const unsigned char*)data;

    switch(len & 7)
    {
      case 7: h ^= uint64_t(data2[6]) << 48;
      case 6: h ^= uint64_t(data2[5]) << 40;
      case 5: h ^= uint64_t(data2[4]) << 32;
      case 4: h ^= uint64_t(data2[3]) << 24;
      case 3: h ^= uint64_t(data2[2]) << 16;
      case 2: h ^= uint64_t(data2[1]) << 8;
      case 1: h ^= uint64_t(data2[0]);
        h *= m;
    };

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
  }

  // General purpose fast hash for internal use in hash tables. Is not guaranteed to be
  // stable across releases, runs, or on different architectures, so do not use
  // in file formats / APIs that require it.
  static inline uint64_t hash(const void * key, size_t len, uint64_t seed = 0) {
    // return MurmurHash64A(key, len, seed);
    // return fvn1a(key, len, FVN_Seed+seed);
    // return XXH64(key, len, seed);
    return XXH3_64bits_withSeed(key, len, seed);
  }

};

}