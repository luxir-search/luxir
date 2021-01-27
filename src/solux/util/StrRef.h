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

namespace solux {

// the general implementation to use for a non-owning Term references.
class PackedTerm;

using TermRef = PackedTerm;


// most x86_64 processors only use the lower 48 bits of pointers.  The upper bits (48 through 63) must all be 1 or 0
// and must match the sign of the 47th bit (the processor will throw an exception if this is not the case)
class StrRef {
  int64_t x;
public:
  static const uint32_t SIZE_BITS = 16;
  static const uint32_t MAX_SIZE = (1 << SIZE_BITS) - 1;

  static uint32_t getMaxSize(uint32_t size) { return size; }

  static uint32_t getExactSize(uint32_t size) { return size; }

  // returns the number of bytes written to the target
  static int write(char *target, const void *data, int sz) {
    memcpy(target, data, (size_t) sz);
    return sz;
  }

  StrRef() {}

  // expert: should already point to an instance of this type
  void init(void *data, uint32_t size) {
    assert((size & 0xffff0000) == 0);
    x = (reinterpret_cast<int64_t>(data) << SIZE_BITS) + size;
  }

  // expert: should already point to an instance of this type
  StrRef(void *data, uint32_t size) {
    init(data, size);
  }

  StrRef(MemPool &target, const void *data, uint32_t len) {
    assert((len & 0xffff0000) == 0);
    auto p = target.allocate(len);
    memcpy(p, data, len);
    init(p, len);
  }


  void *ptr() const {
    // Do a signed shift so we get the correct sign extension (all bits above the 48th bit
    // must match the 48th bit).  Most operating systems I know of use the "0" half of the address
    // space for user-space, but the signed extension is free anyway for our purposes here (unless
    // directly storing the top 16 bits is cheaper since no shift is needed?)
    return reinterpret_cast<void *>(x >> SIZE_BITS);
  }

  uint32_t size() const { return (uint16_t) x; }

  bool operator==(const StrRef &other) const {
    return size() == other.size() && memcmp(ptr(), other.ptr(), size());
  }

  bool isNull() const { return x == 0; }

  size_t hash_value() const {
    return Hash::hash(ptr(), size());
  }

  // compare to raw bytes
  bool equals(const void *data, int len) const {
    return size() == len && memcmp(ptr(), data, (size_t) len) == 0;
  }

  int compare(const StrRef &other) const {
    auto sz1 = size();
    auto sz2 = other.size();

    int cmp = memcmp(ptr(), other.ptr(), (size_t) std::min(sz1, sz2));
    return cmp != 0 ? cmp : ((int) sz1 - (int) sz2); // IMPORTANT: cast sizes to signed values so we can get negatives
  }

  bool operator<(const StrRef &other) const {
    return compare(other) < 0;
  }


  friend std::ostream &operator<<(std::ostream &out, const StrRef &term) {
    if (term.isNull()) {
      out << "(null)";
    } else {
      out.write((const char *) term.ptr(), term.size());
    }
    return out;
  }
};


// A term has a byte of size (0-255) followed directly by the data.
// NOTE: if this is overlaid over zeroes, isNull() will return true.
// By default, *no* initialization is done.
class PackedTerm {
  char *ptr_;
public:
  static uint32_t getMaxSize(uint32_t size) { return size + 1; }

  static uint32_t getExactSize(uint32_t size) { return size + 1; }

  // returns the number of bytes written to the target... either sz+1 or sz+2
  inline static uint32_t write(char *target, const void *data, uint32_t sz) {
    target[0] = sz;
    memcpy(target + 1, data, (size_t) sz);
    return sz + 1;
  }

  inline static char *write(MemPool &targetPool, const void *data, uint32_t sz) {
    auto totalSz = getExactSize(sz);
    auto target = targetPool.allocate(totalSz);
    write(target, data, sz);
    return target;
  }

  // TODO: keep this a trivial class that doesn't initialize itself?
  PackedTerm() {}

  PackedTerm(MemPool &target, const void *data, uint32_t len) {
    ptr_ = write(target, data, len);
  }
  PackedTerm(MemPool &target, std::string_view s) {
    ptr_ = write(target, s.data(), s.size());
  }

  // expert: should already point to an instance of this type
  void init(void *ptr, uint32_t size) {
    ptr_ = reinterpret_cast<char *>(ptr);
  }

  // expert: should already point to an instance of this type
  // TODO: make this somehow harder to accidentally use!
  explicit PackedTerm(void *ptr) : ptr_(reinterpret_cast<char *>(ptr)) {}

  explicit PackedTerm(void *ptr, uint32_t size) : ptr_(reinterpret_cast<char *>(ptr)) {}

  // expert: a pointer to the start of the data... not to the first byte of the string!
  void *ptr() const { return (void *) ptr_; }

  // the number of bytes in the value, not including the bytes to encode the length
  uint32_t size() const noexcept {
    return *(unsigned char *) ptr_;
  }

  char *data() const noexcept {
    return ptr_ + 1;
  }

  // returns the unpacked term as a pair of pointer,size
  std::tuple<const char *, uint32_t> unpack() const {
    return {ptr_ + 1, size()};
  };

  // expert: Up to you not to misuse this.
  void setSize(uint32_t sz) { ptr_[0] = (char) sz; }

// TODO: do this in a more standard way
  std::size_t hash_value() const noexcept {
    return Hash::hash(ptr_ + 1, size());
  }

  bool isNull() const { return ptr_ == nullptr; }

  // compare to raw bytes
  bool equals(const void *ptr, int len) const {
    auto sz = size();
    return sz == len && memcmp(ptr_ + 1, ptr, (size_t) len) == 0;
  }

  // size of both the length and the data
  uint32_t memorySize() const {
    return size() + 1;
  }

  bool operator==(const PackedTerm &other) const {
    auto sz1 = size();
    auto sz2 = other.size();

    if (sz1 != sz2) {
      return false;
    }

    // TODO: make sure memcmp is faster/equal to a loop for likely small strings
    return memcmp(ptr_ + 1, other.ptr_ + 1, sz1) == 0;
  }

  friend std::ostream &operator<<(std::ostream &out, const PackedTerm &term) {
    if (term.isNull()) {
      out << "(null)";
    } else {
      // TODO: perhaps escape unprintable bytes?
      auto[p, sz] = term.unpack();
      out.write(p, sz);
    }
    return out;
  }

  explicit operator std::string_view() const { return std::string_view(data(), size()); }
};

// NOTE: comparators with const char* are not supported as we are dealing with binary data
// which has explicit lengths (and accidental use would lead to bugs)

inline int operator<=>(const PackedTerm &a, const PackedTerm &b) {
  int datacmp = memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
  return (datacmp != 0) ? datacmp : ((int) a.size() - (int) b.size());
}

template<typename StringType>
// StringType just needs size() and data().... which std::string and std::string_view both have.
inline bool operator==(const PackedTerm &p, const StringType &s) {
  auto[data, sz] = p.unpack();
  if (sz != (int) s.size()) return false;
  return memcmp(data, s.data(), sz) == 0;
}

template<typename StringType>
inline bool operator==(const StringType &s, const PackedTerm &p) {
  return p == s;
}

template<typename StringType>
inline int operator<=>(const PackedTerm &p, const StringType &s) {
  auto[data, sz] = p.unpack();
  int datacmp = memcmp(data, s.data(), std::min((int) sz, (int) s.size()));
  return (datacmp != 0) ? datacmp : ((int) sz - (int) s.size());
}

// Hashers and Comparators to use for heterogeneous lookup.
// don't do const char* versions since we are dealing with binary data
struct PackedTermHash {
  using is_transparent = void;
  size_t operator()(const char* data, size_t len) const {
    return Hash::hash(data, len);
  }
  size_t operator()(const PackedTerm& term) const {
    return (*this)(term.data(), term.size());
  }
  size_t operator()(const std::string& str) const {
    return (*this)(str.data(), str.size());
  }
  size_t operator()(const std::string_view& str) const {
    return (*this)(str.data(), str.size());
  }
};

struct PackedTermEqual {
  using is_transparent = void;
  bool operator()(const PackedTerm& lhs, const PackedTerm& rhs) const noexcept {
    return lhs == rhs;
  }
  bool operator()(const PackedTerm& lhs, const std::string_view& rhs) const noexcept {
    return lhs == rhs;
  }
  bool operator()(const std::string_view& lhs, const PackedTerm& rhs) const noexcept {
    return lhs == rhs;
  }
  bool operator()(const PackedTerm& lhs, const std::string& rhs) const noexcept {
    return lhs == rhs;
  }
  bool operator()(const std::string& lhs, const PackedTerm& rhs) const noexcept {
    return lhs == rhs;
  }
  bool operator()(const std::string_view& lhs, const std::string_view& rhs) const noexcept {
    return lhs == rhs;
  }
};


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

  static_assert(sizeof(u) <= 16);

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

  const char *data() const {
    return isLocal() ? u.local : u.remote.ptr;
  }

  // returns the pointer,size pair
  std::tuple<const char *, uint32_t> unpack() const {
    if (u.local[size_idx]) {
      return {u.local, u.local[size_idx] + 1};
    } else {
      return {u.remote.ptr, u.remote.size};
    }
  };

  bool operator==(const sso_stringview &other) const {
    // code if we had 64 bit size field
    // if (u.remote.size != other.u.remote.size) return false;

    // compare both size and hash code at once.
    if (*(reinterpret_cast<const uint64_t *>(this)) != *(reinterpret_cast<const uint64_t *>(&other))) return false;

    // at this point, either remote sizes match, or first 8 bytes of local sizes match.
    if (u.remote.ptr == other.u.remote.ptr)
      return true;  // if true, either pointing at same remote string, or all bytes match of local strings.
    // only way for strings to match now is if they are both remote.
    if (isLocal() || other.isLocal()) return false;
    // if one of the pointers was null, then the size would not have matched (i.e. no null check needed)
    return memcmp(u.remote.ptr, other.u.remote.ptr, u.remote.size) == 0;
  }

  int operator<=>(const sso_stringview &other) const {
    auto[a, alen] = this->unpack();
    auto[b, blen] = other.unpack();
    int datacmp = memcmp(a, b, std::min(alen, blen));
    return (datacmp != 0) ? datacmp : ((int) alen - (int) blen);
  }


  // TODO: constructors that point to a pool, copy operators that point to a pool
  // and only copy if the string can't be inlined (isLocal==true).

};

// Another string alternative could inline up to 7 byte strings in the pointer

} // end namespace


namespace std {
template<>
struct hash<solux::PackedTerm> {
  typedef solux::PackedTerm argument_type;
  typedef std::size_t result_type;

  result_type operator()(const argument_type &val) const {
    return val.hash_value();
  }
};
}

namespace std {
template<>
struct hash<solux::StrRef> {
  typedef solux::StrRef argument_type;
  typedef std::size_t result_type;

  result_type operator()(const argument_type &val) const {
    return val.hash_value();
  }
};
}
