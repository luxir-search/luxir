#pragma once

#include "luxir/util/MemPool.h"
#include "luxir/util/StrRef.h"

namespace luxir {

// A hash table for terms, using TermRef as the key.
template<class T>
class TermHash {
  void newTable(unsigned newSize);

public:
  typedef T value_type;
  typedef std::pair<TermRef, T> composite_type;

  composite_type *table_;
  MemPool &pool_;
  int elements_ = 0;   // how many slots used
  int capacity_;       // how many slots may be used
  int mask_;           // mask for power-of-two hash
  unsigned tableSize_; // size of the hash table, always a power of two

  TermHash(MemPool &pool, unsigned initialSizePowerOfTwo) : pool_(pool) {
    newTable(initialSizePowerOfTwo);
  }

  ~TermHash();

  int size() { return elements_; }

  T &lookup(const char *ptr, int sz) {
    int hash = (int) Hash::hash(ptr, sz);
    int slot = hash;
    for (;;) {
      slot = slot & mask_;
      auto v = table_ + slot;
      if (v->first.isNull() || v->first.equals(ptr, sz)) {
        return v->second;
      }
      slot++;
    }
  }

  void rehash();

  // Returns a pointer to the current slot or adds a new slot.
  // A Rehash will move the slot, so do not use the pointer after other TermHash operations.
  // TODO: should we return V& instead like []
  composite_type &lookupOrAdd(const char *ptr, int sz) {
    if (elements_ >= capacity_) {
      rehash();
    }
    int hash = (int) Hash::hash(ptr, sz);
    int slot = hash;
    for (;;) {
      slot = slot & mask_;
      composite_type &v = table_[slot];
      if (v.first.isNull()) {
        elements_++;
        // unsigned char* dest = pool_.writeStr((const unsigned char*)ptr, sz);
        // v.first = reinterpret_cast<PackedTerm&>(dest);
        // v.first = pool_.writeStr((const unsigned char*)ptr, sz);
        v.first = TermRef(pool_, ptr, sz);
        new(&v.second) T();  // invoke default constructor?
        return v;
      } else if (v.first.equals(ptr, sz)) {
        return v;
      }
      slot++;
    }
  }

  template<class CreateFunctor>
  composite_type &lookupOrAdd(const char *ptr, int sz, CreateFunctor createFunctor) {
    if (elements_ >= capacity_) {
      rehash();
    }
    int hash = (int) Hash::hash(ptr, sz);
    int slot = hash;
    for (;;) {
      slot = slot & mask_;
      composite_type &v = table_[slot];
      if (v.first.isNull()) {
        elements_++;
        // unsigned char* dest = pool_.writeStr((const unsigned char*)ptr, sz);
        // v.first = reinterpret_cast<PackedTerm&>(dest);
        v.first = TermRef(pool_, ptr, sz);
        createFunctor(v);
        return v;
      } else if (v.first.equals(ptr, sz)) {
        return v;
      }
      slot++;
    }
  }

  T &get(const char *ptr, int sz) {
    return lookupOrAdd(ptr, sz).second;
  }

  void emplace(const char *ptr, int sz, const T &&v) {
    // lookupOrAdd(ptr, sz).second = v;
    lookupOrAdd(ptr, sz).second = std::move(v);
  }

  // FUTURE: try robinhood hashing?


};

template<class T>
void TermHash<T>::newTable(unsigned newSize) {
  assert(newSize > 0 && std::has_single_bit(newSize));  // has_single_bit is the same as being a power of two

  // this was often twice as fast in some cases - zeroing is not as well optimized it seems
  table_ = reinterpret_cast<TermHash<T>::composite_type *>( new char[newSize * sizeof(TermHash<T>::composite_type)]());
  capacity_ = newSize - (newSize >> 2);  // .75 load factor
  // capacity_ = newSize - (newSize >> 1);  // .5 load factor
  // capacity_ = newSize - (newSize >> 2) - (newSize >> 3);  // .625 load factor

  mask_ = newSize - 1;
  tableSize_ = newSize;
}


template<class T>
void TermHash<T>::rehash() {
  auto oldTable = table_;
  auto oldTableSize = tableSize_;
  newTable(tableSize_ << 1);

  for (auto i = 0; i < oldTableSize; i++) {
    auto oldslot = oldTable + i;
    if (!oldslot->first.isNull()) {
      int hash = static_cast<int>( oldslot->first.hashcode());  // TODO: store this?
      int slot = hash;
      for (;;) {
        slot = slot & mask_;
        composite_type *newslot = table_ + slot;
        if (newslot->first.isNull()) {
          *newslot = std::move(*oldslot);
          break;
        }
        slot++;
      }
    }
  }

  delete[] reinterpret_cast<char *>(oldTable);
}

template<class T>
TermHash<T>::~TermHash() {
  delete[] reinterpret_cast<char *>(table_);
}

} // end namespace