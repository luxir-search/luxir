#pragma once

#include <stddef.h>
#include <stdint.h>
#include <iostream>
#include <vector>
#include <memory.h>

// NOTE: this is better than including xxhash.h since it enables inline. Inverter performance equal to
// fvn1a when inlined.  25% slower if not inlined.
#include <xxh3.h>

namespace solux {

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

// Returns true if x is a power of two. Will also return true for x==0, so check for that separately if needed.
inline bool isPowerOfTwo(int x) {
  return ((x > 0) && !(x & (x - 1)));
}

inline bool isPowerOfTwo(unsigned int x) {
  return ((x != 0) && !(x & (x - 1)));
}


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

  static uint32_t hash(const void *ptr, int length, uint64_t seed = 0) {
    // return fvn1a(ptr, length, FVN_Seed+seed);
    return (uint32_t) XXH64(ptr, length, seed);
  }

  static uint64_t hash64(const void *ptr, int length, uint64_t seed = 0) {
    return XXH64(ptr, length, seed);
  }

};

}