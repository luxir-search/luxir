#pragma once

//
// StrRef was the same speed as PackedTerm when all terms were length 16
// When length was changed to 4+(i&0xf) (i.e. between 4 and 19 bytes), things
// got slower for both (branch prediction on hashcode?), but StrRef was about
// 18% faster.  Is this because the length can be compared before any memory
// is fetched?  If so, that's not a realistic speedup due to the unrealistic distribution
// of term lengths.
//
// Another alternative: store upper bits of hash code (i.e. those not used to find bucket)
// in the pointer instead for a fast != (or store hash along with key in the table for faster rehashing?)
//  Do probes based on hash (in a separate column) until an equal hash code is found, then check key?
//  But from a cache perspective, having the hash next to the key pointer should be more efficient.
// or just decrease the average load factor in the hash table to reduce collisions???
//
// TODO: try a bigger string with a pointer, length, and hash?
// Making the load factor of the table much bigger (.875) only decreased performance by 3%... this
// is probably because collisions are unrealistically too cheap because of the
// unrealistic uniform distribution of lengths.  Need a more realistic test!
//
// It's also very possible that rehashing is taking up a lot of time.
// 1M keys starting at table size of 4, PackedTerm impl
//   build keys/sec=3.90999e+06 lookup keys/sec=7.60231e+06
// 1M keys starting at table size of 4, StrRef impl:
//   build keys/sec=4.47021e+06 lookup keys/sec=9.4353e+06
// 1M keys starting at table size of 1<<21 (no resize necessary)
//   build keys/sec=9.37673e+06 lookup keys/sec=9.18923e+06
//

#include <memory>
#include <iostream>
#include <string.h>
#include <vector>
#include <tuple>
#include <algorithm>
#include <assert.h>
#include "solux_util.h"
#include "MemPool.h"



// the general implementation to use for a non-owning Term references.
class PackedTerm;
using TermRef = PackedTerm;


// most x86_64 processors only use the lower 48 bits of pointers.  The upper bits (48 through 63) must all be 1 or 0
// and must match the sign of the 47th bit (the processor will throw an exception if this is not the case)
class StrRef {
  int64_t x;
public:
  static const uint32_t SIZE_BITS=16;
  static const uint32_t MAX_SIZE=(1<<SIZE_BITS)-1;

  static uint32_t getMaxSize(uint32_t size) { return size; }
  static uint32_t getExactSize(uint32_t size) { return size; }

  // returns the number of bytes written to the target
  static int write(char* target, const void* data, int sz) {
    memcpy(target, data, (size_t)sz);
    return sz;
  }

  StrRef() {}

  // expert: should already point to an instance of this type
  void init(void* data, uint32_t size) {
    assert( (size & 0xffff0000)==0 );
    x = (reinterpret_cast<int64_t>(data) << SIZE_BITS) + size;
  }

  // expert: should already point to an instance of this type
  StrRef(void* data, uint32_t size) {
    init(data, size);
  }
  StrRef(MemPool& target, const void* data, uint32_t len) {
    assert( (len & 0xffff0000)==0 );
    auto p = target.allocatePtr(len);
    memcpy(p, data, len);
    init(p, len);
  }


  void* ptr() const {
    // Do a signed shift so we get the correct sign extension (all bits above the 48th bit
    // must match the 48th bit).  Most operating systems I know of use the "0" half of the address
    // space for user-space, but the signed extension is free anyway for our purposes here (unless
    // directly storing the top 16 bits is cheaper since no shift is needed?)
    return reinterpret_cast<void*>(x >> SIZE_BITS);
  }

  uint32_t size() const { return (uint16_t)x; }
  bool operator==(const StrRef& other) const {
    return size() == other.size() && memcmp(ptr(), other.ptr(), size());
  }

  bool isNull() const { return x==0; }

  uint64_t hashcode() const {
    return Hash::hash(ptr(), size());
  }

  // compare to raw bytes
  bool equals(const void* data, int len) const {
    return size() == len && memcmp(ptr(), data, (size_t)len)==0;
  }

  int compare(const StrRef& other) const {
    auto sz1 = size();
    auto sz2 = other.size();

    int cmp = memcmp(ptr(), other.ptr(), (size_t)std::min(sz1, sz2));
    return cmp != 0 ? cmp : ((int)sz1 - (int)sz2); // IMPORTANT: cast sizes to signed values so we can get negatives
  }

  bool operator<(const StrRef& other) const {
    return compare(other) < 0;
  }


  friend std::ostream& operator<< (std::ostream &out, const StrRef &term) {
    if (term.isNull()) {
      out << "(null)";
    } else {
      out.write((const char *)term.ptr() , term.size());
    }
    return out;
  }
};



// The length is part of the data (a one or 2 byte prefix, supporting sizes up to 32K)
class PackedTerm {
  // TODO: could also just have a char[1] data member here and use the address of that...
  // then instead of casting a pointer to PackedTerm, we'd cast a pointer to a pointer-to-PackedTerm???
  const char* ptr_;

public:
  static uint32_t getMaxSize(uint32_t size) { return size + 2; }
  static uint32_t getExactSize(uint32_t size) { return ((size < 128) ? 1 : 2) + size; }

  // returns the number of bytes written to the target... either sz+1 or sz+2
  inline static int write(char* target, const void* data, int sz) {
    int sizeBytes;
    if (sz<128) {
      target[0] = (char)sz;
      sizeBytes = 1;
    } else {
      target[0] = (char)(sz | 0x80);
      target[1] = (char)(sz>>7);
      sizeBytes = 2;
    }
    memcpy(target+sizeBytes, data, (size_t)sz);
    return sz + sizeBytes;
  }
  inline static const char* write(MemPool& targetPool, const void* data, uint32_t sz) {
    auto totalSz = getExactSize(sz);
    auto target = targetPool.allocatePtr(totalSz);
    write(target, data, sz);
    return target;
  }

  // this version seemed a little faster for clang, but not for g++
  inline static const char* write2(MemPool& targetPool, const void* data, uint32_t sz) {
    targetPool.reserve(getMaxSize(sz));
    auto target = targetPool.ptr();
    auto sizeOut = write(target, data, sz);
    targetPool.pos_ += sizeOut;
    return target;
  }

  // TODO: keep this a trivial class that doesn't initialize itself?
  PackedTerm() {}
  PackedTerm(MemPool& target, const void* data, uint32_t len) {
    ptr_ = write(target, data, len);
  }

  // expert: should already point to an instance of this type
  void init(void* ptr, uint32_t size) {
    ptr_ = reinterpret_cast<const char*>(ptr);
  }

  // expert: should already point to an instance of this type
  // TODO: make this somehow harder to accidentally use!
  PackedTerm(void* ptr) : ptr_(reinterpret_cast<const char*>(ptr)) { }
  PackedTerm(void* ptr, uint32_t size) : ptr_(reinterpret_cast<const char*>(ptr)) { }

  // expert: a pointer to the start of the data... not to the first byte of the string!
  void* ptr() { return (void*)ptr_; }



// TODO: do this in a more standard way
  uint64_t hashcode() const {
    int sz = ptr_[0];
    int off=1;
    if (sz & 0x80) {  // this branch normally won't be taken
      sz = (sz & 0x7f) | (ptr_[1]<<7);
      off=2;
    }
    return Hash::hash(ptr_+off, sz);
  }

  bool isNull() const { return ptr_ == nullptr; }


  // compare to raw bytes
  bool equals(const void* ptr, int len) const {
    int sz = ptr_[0];
    int off=1;
    if (sz & 0x80) {  // this branch normally won't be taken
      sz = (sz & 0x7f) | (ptr_[1]<<7);
      off=2;
    }
    return sz == len && memcmp(ptr_+off, ptr, (size_t)len)==0;
  }

  // returns the unpacked term as a pair of pointer,size
  std::tuple<const char*, int> unpack() const {
    int sz = ptr_[0];
    int off=1;
    if (sz & 0x80) {  // this branch normally won't be taken
      sz = (sz & 0x7f) | (ptr_[1]<<7);
      off=2;
    }
    return {ptr_+off, sz};
  };


  // the number of bytes in the value, not including the bytes to encode the length
  int size() const {
    int sz = ptr_[0];
    if (sz & 0x80) {  // this branch normally won't be taken
      sz = (sz & 0x7f) | (ptr_[1] << 7);
    }
    return sz;
  }

  bool operator==(const PackedTerm& other) const {
    // TODO: try just using unpack, verify no slowdown.  Once we use a hash table
    // that embeds part of the hash code, this should normally return true at this
    // point any sort of potential short circuiting won't matter here.

    int sz1 = ptr_[0];
    int off=1;
    if (sz1 & 0x80) {  // this branch normally won't be taken
      sz1 = (sz1 & 0x7f) | (ptr_[1]<<7);
      off=2;
    }

    int sz2 = other.ptr_[0];
    int off2=1;
    if (sz1 & 0x80) {  // this branch normally won't be taken
      sz1 = (sz1 & 0x7f) | (other.ptr_[1]<<7);
      off2=2;
    }
    if (sz1 != sz2) {
      return false;
    }

    // TODO: make sure memcmp is faster/equal to a loop for likely small strings
    return memcmp(ptr_+off, other.ptr_+off, sz1) == 0;
  }

  int compare(const PackedTerm& other) const {
    int sz1 = ptr_[0];
    int off=1;
    if (sz1 & 0x80) {  // this branch normally won't be taken
      sz1 = (sz1 & 0x7f) | (ptr_[1]<<7);
      off=2;
    }

    int sz2 = other.ptr_[0];
    int off2=1;
    if (sz1 & 0x80) {  // this branch normally won't be taken
      sz1 = (sz1 & 0x7f) | (other.ptr_[1]<<7);
      off2=2;
    }

    // TODO: make sure memcmp is faster/equal to a loop for likely small strings
    int cmp = memcmp(ptr_+off, other.ptr_+off2, (size_t)std::min(sz1, sz2));
    return cmp != 0 ? cmp : (sz1-sz2);
  }

  bool operator<(const PackedTerm& other) const {
    return compare(other) < 0;
  }


  friend std::ostream& operator<< (std::ostream &out, const PackedTerm &term) {
    if (term.isNull()) {
      out << "(null)";
    } else {
      auto [p,sz] = term.unpack();
      out.write((const char *) p, sz);
    }
    return out;
  }

};


namespace std {
template<>
struct hash<PackedTerm> {
  typedef PackedTerm argument_type;
  typedef std::size_t result_type;

  result_type operator()(const argument_type &val) const {
    return val.hashcode();
  }
};
}
namespace std {
template<>
struct hash<StrRef> {
  typedef StrRef argument_type;
  typedef std::size_t result_type;

  result_type operator()(const argument_type &val) const {
    return val.hashcode();
  }
};
}

//
// TODO: experimental and in progress string_view with short string optimization (can store strings of length 15 inline)
// May result in better cache locality, esp when sorting a large list of strings, provided many are under length 15.
// Would hurt otherwise (like a uuid that is always over.)
// We could have a separate string pool to make it easier to keep the lifetime of a bunch of strings together.
// TODO: how to tell compiler that this is trivially relocatable?
//
class sso_stringview {
  // NOTE: this class
  struct remote_type {
    uint32_t size;
    uint32_t cached_hash;  // Should we do this? Eagerly?
    const char *ptr;  // little-endian, the last byte (high byte) will be 0 with normal addressing
  };

  union {
    remote_type remote;
    char local[sizeof(remote)];
  } u;

  static_assert(sizeof(u)<=16);

  // Look at this byte to find the local size plus 1 (to enable storing a zero lengh string)
  // We could also support the idea of a null value (if remote.ptr is 0)
  constexpr static uint64_t size_idx = sizeof(remote_type) - 1;

  bool isLocal() const {
    return u.local[size_idx];
  }

public:

  uint32_t size() const {
    return u.local[size_idx] ? u.local[size_idx] + 1 : u.remote.size;
  }

  const char* data() const {
    return isLocal() ? u.local : u.remote.ptr;
  }

  // returns the pointer,size pair
  std::tuple<const char*, uint32_t> unpack() const {
    if (u.local[size_idx]) {
      return {u.local, u.local[size_idx] + 1};
    } else {
      return {u.remote.ptr, u.remote.size};
    }
  };

  bool operator==(const sso_stringview& other) const {
    // code if we had 64 bit size field
    // if (u.remote.size != other.u.remote.size) return false;

    // compare both size and hash code at once.
    if ( *(reinterpret_cast<const uint64_t*>(this)) != *(reinterpret_cast<const uint64_t*>(&other)) ) return false;

    // at this point, either remote sizes match, or first 8 bytes of local sizes match.
    if (u.remote.ptr == other.u.remote.ptr) return true;  // if true, either pointing at same remote string, or all bytes match of local strings.
    // only way for strings to match now is if they are both remote.
    if (isLocal() || other.isLocal()) return false;
    // if one of the pointers was null, then the size would not have matched (i.e. no null check needed)
    return memcmp(u.remote.ptr, other.u.remote.ptr, u.remote.size) == 0;
  }

  int operator<=>(const sso_stringview& other) const {
    auto [a, alen] = this->unpack();
    auto [b, blen] = other.unpack();
    int datacmp = memcmp(a, b, std::min(alen, blen));
    return (datacmp != 0) ? datacmp : ((int)alen - (int)blen);
  }


  // TODO: constructors that point to a pool, copy operators that point to a pool
  // and only copy if the string can't be inlined (isLocal==true).

};

// Another string alternative could inline up to 7 byte strings in the pointer

