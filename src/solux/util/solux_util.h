#pragma once

#include <stddef.h>
#include <stdint.h>
#include <iostream>
#include <vector>
#include <memory.h>

// gcc and msvc have different ways of specifying packing of structs :-(
// use SOLUX_PACKED_START class X{} SOLUX_PACKED_END;
#ifdef __GNUC__
#define SOLUX_PACKED( __Declaration__ ) __Declaration__ __attribute__((__packed__))
#define SOLUX_PACKED_START ;
#define SOLUX_PACKED_END __attribute__((__packed__))
#endif

#ifdef _MSC_VER
#define SOLUX_PACKED( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop))
#define SOLUX_PACKED_START __pragma(pack(push,1))
#define SOLUX_PACKED_END __pragma(pack(pop))
#endif

template <typename... Args> inline void unused(Args&&...) {}



// java compatible types?
typedef int8_t byte;

// Returns true if x is a power of two. Will also return true for x==0, so check for that separately if needed.
inline bool isPowerOfTwo(int x)
{
  return ((x > 0) && !(x & (x - 1)));
}

inline bool isPowerOfTwo(unsigned int x)
{
  return ((x != 0) && !(x & (x - 1)));
}

// Although not the highest quality PRNG, xorshift64 simplicity is good for matching random sequences in different languages.
// Do not pass 0, and it is recommended to start with something that has a lot of bits set, otherwise it takes a while to
// build up to that.
inline uint64_t xorshift(uint64_t x) {
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}
// TODO: implement wyrand... requires 64*64 bit -> 128 result multiply though (harder to port though)


// generic vector ostream
template <typename T>
std::ostream& operator<< (std::ostream& out, const std::vector<T>& v) {
  out << '[';
  for (size_t i=0; i < v.size(); i++) {
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



// from MetroHash64

// rotate right idiom recognized by most compilers
inline static uint64_t rotate_right(uint64_t v, unsigned k)
{
  return (v >> k) | (v << (64 - k));
}

// unaligned reads, fast and safe on Nehalem and later microarchitectures
inline uint64_t read_u64(const void * const ptr)
{
  return static_cast<uint64_t>(*reinterpret_cast<const uint64_t*>(ptr));
}

inline uint64_t read_u32(const void * const ptr)
{
  return static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(ptr));
}

inline uint64_t read_u16(const void * const ptr)
{
  return static_cast<uint64_t>(*reinterpret_cast<const uint16_t*>(ptr));
}

inline uint64_t read_u8 (const void * const ptr)
{
  return static_cast<uint64_t>(*reinterpret_cast<const uint8_t *>(ptr));
}


// todo: get XXH3
// todo: namespace
class Hash
{
private:
  static const uint64_t k0 = 0xD6D018F5;
  static const uint64_t k1 = 0xA2AA033B;
  static const uint64_t k2 = 0x62992FC1;
  static const uint64_t k3 = 0x30BC5B29;

  static constexpr uint32_t FVN_Prime = 0x01000193; //   16777619
  static constexpr uint32_t FVN_Seed  = 0x811C9DC5; // 2166136261

public:
  /**
  static uint64_t mix(uint64_t val) {
    h += read_u64(ptr) * k3; ptr += 8;
    h ^= rotate_right(h, 55) * k1;
    h ^= rotate_right(h, 28);
    h *= k0;
    h ^= rotate_right(h, 29);
  }
   **/

  /// fnv1a hash, descent for hash tables with short keys.
  // We could do better if we knew it was safe to read up to 8 bytes before/after the data...
  // see wyhash (safety=0) and fvn1a-pippin
  static inline uint32_t fvn1a(const void* data, size_t numBytes, uint32_t hash = FVN_Seed)
  {
    auto ptr = (const unsigned char*)data;
    while (numBytes--)
      hash = (*ptr++ ^ hash) * FVN_Prime;
    return hash;
  }


// fasthash
// Compression function for Merkle-Damgard construction.
// This function is generated using the framework provided.
  static inline uint64_t mix(uint64_t h) {
    (h) ^= (h) >> 23;
    (h) *= 0x2127599bf4325c37ULL;
    (h) ^= (h) >> 47;
    return h;
  }

  // non-inlined version
  static uint64_t fasthash64_func(const void *buf, size_t len, uint64_t seed);

  static uint64_t fasthash64(const void *buf, size_t len, uint64_t seed)
  {
    const uint64_t    m = 0x880355f21e6d1965ULL;
    const uint64_t *pos = (const uint64_t *)buf;
    const uint64_t *end = pos + (len / 8);
    const unsigned char *pos2;
    uint64_t h = seed ^ (len * m);
    uint64_t v;

    while (pos != end) {
      v  = *pos++;
      h ^= mix(v);
      h *= m;
    }

    pos2 = (const unsigned char*)pos;
    v = 0;

    switch (len & 7) {
      case 7: v ^= (uint64_t)pos2[6] << 48;
      case 6: v ^= (uint64_t)pos2[5] << 40;
      case 5: v ^= (uint64_t)pos2[4] << 32;
      case 4: v ^= (uint64_t)pos2[3] << 24;
      case 3: v ^= (uint64_t)pos2[2] << 16;
      case 2: v ^= (uint64_t)pos2[1] << 8;
      case 1: v ^= (uint64_t)pos2[0];
        h ^= mix(v);
        h *= m;
    }

    return mix(h);
  }

  static uint64_t ycs_hash(const void *buf, size_t len, uint64_t seed) {
    const unsigned char *ptr = (const unsigned char*)buf;

    uint64_t v = 0;
    switch(len) {
      case 4: v = read_u32(ptr); break;
      case 3: v = (uint64_t)ptr[2] << 16;
      case 2: v ^= read_u16(ptr); break;
      case 1: v = (uint64_t)ptr[0]; break;
      // default: return metro_hash(ptr, len, seed);
      default: return fvn1a(ptr, len, seed);
    }
    return mix(v);
  }


  static uint64_t metro_hash(const void* ptr, int length, uint64_t seed=0);

  static uint32_t hash(const void* ptr, int length, uint64_t seed=FVN_Seed) {
    return fvn1a(ptr, length, seed);
    // return (uint32_t)metro_hash(ptr, length, seed);
    // return (uint32_t)ycs_hash(ptr, length, seed);
    // return (uint32_t)fasthash64(ptr, length, seed);
    // return (uint32_t)fasthash64_func(ptr, length, seed);
  }

  // Simple inverter test: (seems like inlining is hurting here?)
  // g++ MB/sec fvn=133 metro=131 fast=122  fast_func=124
  // clang      fvn= 96 metro=110 fast=110  fast_func=113

};

