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



// gcc and msvc have different ways of specifying packing of structs :-(
// use SOLUX_PACKED_START class X{} SOLUX_PACKED_END;
#ifdef __GNUC__
#define SOLUX_PACKED(__Declaration__) __Declaration__ __attribute__((__packed__))
#define SOLUX_PACKED_START ;
#define SOLUX_PACKED_END __attribute__((__packed__))
#endif

#ifdef _MSC_VER
#define SOLUX_PACKED( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop))
#define SOLUX_PACKED_START __pragma(pack(push,1))
#define SOLUX_PACKED_END __pragma(pack(pop))
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