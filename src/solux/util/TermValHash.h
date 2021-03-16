#pragma once

#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"
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
  void initTV(MemPool &pool, const std::string_view& str, Args &&... args) {
    // pool.align(); // Cost=~4 bytes per unique term... doesn't seem to be worth it from benchmarks so far.
    auto target = pool.allocate(getExactSize(str.size()));
    new(target) V(std::forward<Args>(args)...);    // construct the value
    auto strStart = target + sizeof(V);
    TermRef::write(strStart, str.data(), str.size());  // copy the string following the value
    init(strStart, str.size()); // the pointer to *this* compound value is the same as the string, and hence we can thus inherit from the string
  }

  template<typename... Args>
  TermValRef(MemPool &pool, const char *str, unsigned len, Args &&... args) {
    initTV(pool, std::string_view(str,len), std::forward<Args>(args)...);
  }

  template<typename... Args>
  TermValRef(MemPool &pool, const std::string_view& str, Args &&... args) {
    initTV(pool, str, std::forward<Args>(args)...);
  }

  friend std::ostream &operator<<(std::ostream &out, const TermValRef &tv) {
    if (tv.isNull()) {
      out << "(null)";
    } else {
      out << (const TermRef&)tv  // base class string
          << ':'
          << tv.val();
    }
    return out;
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

// A map that is implemented more like a set.
// It wraps a T in a TermValRef, storing them together in the MemPool.
template<class T, class Hasher=PackedTermHash>
class TermValHash {
public:
  using entry_type = TermValRef<T>; // should normally be the size of a single pointer
  using iterator = entry_type*;

private:
  entry_type* table_;
  MemPool& pool_;
  int elements_ = 0;   // how many slots used
  int capacity_;       // how many slots may be used before rehashing
  unsigned tableSize_; // size of the hash table, always a power of two

  void newTable(unsigned newSize);

  void rehash();
  // use stable_partition on the existing memory to sort?

  static inline auto non_null_predicate = [](const entry_type &entry) {
    return !entry.isNull();
  };

public:
  TermValHash(MemPool &pool, unsigned initialSizePowerOfTwo) : pool_(pool) {
    newTable(initialSizePowerOfTwo);
  }

  TermValHash(TermValHash&& other) :
    table_(other.table_), pool_(other.pool_), elements_(other.elements_),
    capacity_(other.capacity_), tableSize_(other.tableSize_)
  {
    other.table_ = nullptr;
  }

  ~TermValHash() {
    free();
  }

  // free up what memory we can early (i.e. before normal destructor would be called)
  void free() {
    if (table_ != nullptr) {
      delete[] reinterpret_cast<char *>(table_);
      table_ = nullptr;
    }
  }

  [[nodiscard]] MemPool& getMemPool() const { return pool_; }

  [[nodiscard]] size_t size() const { return (size_t) elements_; }

  // iterators that only return non-null elements... switch to c++20 ranges when ready?
  auto begin() {
    return boost::make_filter_iterator(non_null_predicate, table_, table_ + tableSize_);
  }

  // TODO: don't know the performance implications of calling this often...
  auto end() {
    return boost::make_filter_iterator(non_null_predicate, table_ + tableSize_, table_ + tableSize_);
  }

  /// memory consumed by this table (does not traverse into values, only includes shallow value size)
  size_t memSize() const {
    return tableSize_ * sizeof(entry_type) + sizeof(TermValHash<T,Hasher>) ;
  }

  template<typename... Args>
  std::pair<iterator, bool> try_emplace(const std::string_view& str, Args &&... args) {
    if (elements_ >= capacity_) {
      rehash();
    }
    auto hash = Hasher()(str);
    auto slot = hash;
    for (;;) {
      slot = slot & (tableSize_ - 1);
      iterator v = table_ + slot;
      if (v->isNull()) {
        elements_++;
        *v = entry_type(pool_, str, std::forward<Args>(args)...);
        return {v, true};
      } else if (*v == str) {
        return {v, false};
      }
      slot++;
    }
  }

  // Some heterogeneous lookup support.  We could support it as a template param if needed.
  template<typename... Args>
  std::pair<entry_type, bool> try_emplace(const std::string& s, Args &&... args) {
    return try_emplace(s.data(), s.size(), std::forward<Args>(args)...);
  }

  // Packs all values into the start of the internal array and returns a pointer to the beginning.
  // Additions, lookups, or other hash operations will be undefined after this point.  The purpose of this
  // method is to avoid an extra (potentially large) allocation just to copy the values to sort.
  // The memory pointed at is still owned by this table and should not be accessed after it has been destructed.
  iterator destructiveCompress() {
    auto sz = size();
    size_t endIdx = tableSize_;
    for (size_t i=0; i<sz; i++) {
      // first find an empty slot
      if (!table_[i].isNull()) continue;
      // now search from the end to find a full slot
      while (table_[--endIdx].isNull());
      // move the occupied slot at the end to be empty slot
      table_[i] = table_[endIdx];
      // not necessary to actually "empty" the occupied slot we copied.
    }
    return table_;
  }

  friend std::ostream& operator<<(std::ostream &out, const TermValHash &tvh) {
    return out << "{size:" << tvh.size() << " loadFactor:" << tvh.size()/(float)tvh.tableSize_ << "}";
  }
};

template<class T, class Hasher>
void TermValHash<T,Hasher>::newTable(unsigned newSize) {
  assert(newSize > 0 && isPowerOfTwo(newSize));

  // this was often twice as fast in some cases - zeroing is not as well optimized for some types it seems
  table_ = reinterpret_cast<TermValHash<T,Hasher>::entry_type *>( new char[newSize * sizeof(TermValHash<T,Hasher>::entry_type)]() );
  capacity_ = newSize - (newSize >> 2);  // .75 load factor
  tableSize_ = newSize;
}


template<class T, class Hasher>
void TermValHash<T,Hasher>::rehash() {
  auto oldTable = table_;
  auto oldTableSize = tableSize_;
  newTable(tableSize_ << 1);

  for (auto i = 0; i < oldTableSize; i++) {
    auto oldslot = oldTable + i;
    if (!oldslot->isNull()) {
      auto hash = Hasher()(*oldslot);
      auto slot = hash;
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

} // end namespace
