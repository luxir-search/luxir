#pragma once

#include "solux/util/MemPool.h"
#include <functional>
#include "boost/iterator/filter_iterator.hpp"

namespace solux {

// TODO: figure out if/how we can use abseil or folly F14, phmap or robin_hood hash maps that have SIMD lookups.
// If we can't use directly, perhaps a hacked version?
// Also phmap::flat_hash_set


// A reference to an adjacent string key and arbitrary value, for better memory locality
// The current implementation stores Val+Key contiguously and points in the middle (to the start of the Key).
//
// One would typically want to define the value type V as packed so there isn't wasted padding
// between the struct and the string.
//
// FUTURE: if we align in the pool, we could use that alignment space after the key... perhaps even round to
// the cache line (64bytes on x86)
//


template<class V>
class TermValRef : public TermRef {
private:

public:
  static unsigned getMaxSize(unsigned strBytes) { return TermRef::getMaxSize(strBytes) + sizeof(V); }

  static unsigned getExactSize(unsigned strBytes) { return TermRef::getExactSize(strBytes) + sizeof(V); }

  // Expert!
  // create a reference to a ValKey pair that already exists in memory.
  // ptr/len refer to the string portion that directly follows the value.
  explicit TermValRef(void *ptr, unsigned len) : TermRef(ptr, len) {}

  V *valPtr() const {
    // return const_cast<V*>( reinterpret_cast<const V*>( (const char*)ptr() - sizeof(V) ) );
    return reinterpret_cast<V *>(ptr()) - 1;
  }

  V &val() const { return *valPtr(); }

  template<typename... Args>
  static TermValRef create(MemPool &pool, const char *str, unsigned len, Args &&... args) {
    pool.align(); // Cost=~4 bytes per unique term
    auto target = pool.allocate(getExactSize(len));
    new(target) V(std::forward<Args>(args)...);    // construct the value
    auto strStart = target + sizeof(V);
    TermRef::write(strStart, str, len);  // copy the string following the value
    return TermValRef(strStart,
                      len);                // the pointer to *this* compound value is the same as the string, and hence we can thus inherit from the string
  }

  template<typename... Args>
  TermValRef(MemPool &pool, const char *str, unsigned len, Args &&... args) {
    pool.align(); // Cost=~4 bytes per unique term
    auto target = pool.allocate(getExactSize(len));
    new(target) V(std::forward<Args>(args)...);    // construct the value
    auto strStart = target + sizeof(V);
    TermRef::write(strStart, str, len);  // copy the string following the value
    init(strStart, len); // this is like a private constructor for the string part.
  }
};


// FUTURE OPT: Sorting the terms when serializing may have pretty bad locality.
// We could use a custom string_view class that is a single pointer that
// inlines (short string optimization) up to 7 bytes
// if we were dealing with a pool that was always 64 byte aligned, then
// we could normally tell without storing any extra info (or we could just use
// the extra bits we know will always be 0)
// Or a custom string_view class could optionally pack up to 15 chars locally.
// Some of this might be very useful for short english words (the majority), but
// not useful with unique ids (which may represent the bulk of unique terms?)


template<class T>
class TermValHash {
private:
  void newTable(unsigned newSize);

  void rehash();
  // use stable_partition on the existing memory to sort?

public:
  typedef T value_type;
  typedef TermValRef<T> entry_type; // should normally be the size of a single pointer
  typedef entry_type *iterator;

  entry_type *table_;
  MemPool &pool_;
  int elements_ = 0;   // how many slots used
  int capacity_;       // how many slots may be used before rehashing
  unsigned tableSize_; // size of the hash table, always a power of two

  TermValHash(MemPool &pool, unsigned initialSizePowerOfTwo) : pool_(pool) {
    newTable(initialSizePowerOfTwo);
  }

  ~TermValHash();

  size_t size() { return (size_t) elements_; }

  static inline auto non_null_predicate = [](const entry_type &entry) {
    return !entry.isNull();
  };

  // iterators that only return non-null elements... switch to c++20 ranges when ready?
  auto begin() {
    return boost::make_filter_iterator(non_null_predicate, table_, table_ + tableSize_);
  }

  auto end() {
    return boost::make_filter_iterator(non_null_predicate, table_ + tableSize_, table_ + tableSize_);
  }


  // Returns the appropriate slot, but does not initialize it if missing (bits should be 0 in that case).
  entry_type &lookup(const char *ptr, int sz) {
    if (elements_ >= capacity_) {
      rehash();
    }
    int hash = (int) Hash::hash(ptr, sz);
    int slot = hash;
    for (;;) {
      slot = slot & (tableSize_ - 1);
      entry_type &v = table_[slot];
      if (v.isNull()) {
        elements_++;
        return v;
      } else if (v.equals(ptr, sz)) {
        return v;
      }
      slot++;
    }
  }


  // Returns the current slot or adds a new slot.
  // Returns true if the item was inserted.
  std::tuple<entry_type, bool> lookupOrAdd(const char *ptr, int sz) {
    if (elements_ >= capacity_) {
      rehash();
    }
    int hash = (int) Hash::hash(ptr, sz);
    int slot = hash;
    for (;;) {
      slot = slot & (tableSize_ - 1);
      entry_type &v = table_[slot];
      if (v.isNull()) {
        elements_++;
        char *valptr = pool_.allocate(entry_type::getExactSize(sz));
        char *keyPtr = valptr + sizeof(value_type);
        TermRef::write(keyPtr, ptr, sz);
        new(&v) entry_type(keyPtr, sz);
        return std::make_tuple(v, true);
      } else if (v.equals(ptr, sz)) {
        return std::make_tuple(v, false);
      }
      slot++;
    }
  }


  template<typename... Args>
  std::pair<entry_type, bool> try_emplace_a(const char *ptr, int sz, Args &&... args) {
    if (elements_ >= capacity_) {
      rehash();
    }
    int hash = (int) Hash::hash(ptr, sz);
    int slot = hash;
    for (;;) {
      slot = slot & (tableSize_ - 1);
      entry_type &v = table_[slot];
      if (v.isNull()) {
        elements_++;
        char *valptr = pool_.allocate(entry_type::getExactSize(sz));
        char *keyPtr = valptr + sizeof(value_type);
        new(valptr) T(std::forward<Args>(args)...);  // construct the T value
        TermRef::write(keyPtr, ptr, sz);      // write the string key directly after the value
        new(&v) entry_type(keyPtr, sz);                 // construct or hash entry
        return {v, true};
      } else if (v.equals(ptr, sz)) {
        return {v, false};
      }
      slot++;
    }
  }

// TODO: need to test these variants when there are many more unique terms (i.e. test the emplace performance... right now
// only the inserted==false test is being tested (same string over and over)
  template<typename... Args>
  std::pair<entry_type, bool> try_emplace(const char *ptr, int sz, Args &&... args) {
    if (elements_ >= capacity_) {
      rehash();
    }
    auto hash = Hash::hash(ptr, sz);
    auto slot = hash;
    for (;;) {
      slot = slot & (tableSize_ - 1);
      entry_type &v = table_[slot];
      if (v.isNull()) {
        elements_++;
        // v = entry_type::create(pool_, ptr, sz, std::forward<Args>(args)...);
        v = entry_type(pool_, ptr, sz, std::forward<Args>(args)...);
        return {v, true};
      } else if (v.equals(ptr, sz)) {
        return {v, false};
      }
      slot++;
    }
  }


  void update(const char *ptr, int sz, std::function<void(T *)> init, std::function<void(T &)> update) {
    if (elements_ >= capacity_) {
      rehash();
    }
    int hash = (int) Hash::hash(ptr, sz);
    int slot = hash;
    for (;;) {
      slot = slot & (tableSize_ - 1);
      entry_type &v = table_[slot];
      if (v.isNull()) {
        elements_++;
        char *valptr = pool_.allocate(entry_type::getExactSize(sz));
        char *keyPtr = valptr + sizeof(value_type);
        TermRef::write(keyPtr, ptr, sz);
        new(&v) entry_type(keyPtr, sz);
        init(reinterpret_cast<value_type *>(valptr));
        return;
      } else if (v.equals(ptr, sz)) {
        update(v.val());
        return;
      }
      slot++;
    }
  }

  template<class ConstructorFunc, class UpdateFunc>
  void updateT(const char *ptr, int sz, const ConstructorFunc &init, const UpdateFunc &update) {
    if (elements_ >= capacity_) {
      rehash();
    }
    int hash = (int) Hash::hash(ptr, sz);
    int slot = hash;
    for (;;) {
      slot = slot & (tableSize_ - 1);
      entry_type &v = table_[slot];
      if (v.isNull()) {
        elements_++;
        char *valptr = pool_.allocate(entry_type::getExactSize(sz));
        char *keyPtr = valptr + sizeof(value_type);
        TermRef::write(keyPtr, ptr, sz);
        new(&v) entry_type(keyPtr, sz);
        init(reinterpret_cast<value_type *>(valptr));
        return;
      } else if (v.equals(ptr, sz)) {
        update(v.val());
        return;
      }
      slot++;
    }
  }


  // FUTURE: try robinhood hashing?
};

template<class T>
void TermValHash<T>::newTable(unsigned newSize) {
  assert(newSize > 0 && isPowerOfTwo(newSize));

  // this was often twice as fast in some cases - zeroing is not as well optimized for some types it seems
  table_ = reinterpret_cast<TermValHash<T>::entry_type *>( new char[newSize * sizeof(TermValHash<T>::entry_type)]() );
  capacity_ = newSize - (newSize >> 2);  // .75 load factor
  // capacity_ = newSize - (newSize >> 1);  // .5 load factor
  // capacity_ = newSize - (newSize >> 2) - (newSize >> 3);  // .625 load factor
  tableSize_ = newSize;
}


template<class T>
void TermValHash<T>::rehash() {
  auto oldTable = table_;
  auto oldTableSize = tableSize_;
  newTable(tableSize_ << 1);

  for (auto i = 0; i < oldTableSize; i++) {
    auto oldslot = oldTable + i;
    if (!oldslot->isNull()) {
      int hash = (int) oldslot->hashcode();
      int slot = hash;
      for (;;) {
        slot = slot & (tableSize_ - 1);
        entry_type *newslot = table_ + slot;
        if (newslot->isNull()) {
          *newslot = *oldslot;
          break;
        }
        slot++;
      }
    }
  }

  delete[] reinterpret_cast<char *>(oldTable);
}

template<class T>
TermValHash<T>::~TermValHash() {
  delete[] reinterpret_cast<char *>(table_);
}

} // end namespace
