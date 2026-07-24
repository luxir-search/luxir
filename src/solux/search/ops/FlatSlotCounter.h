#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "solux/util/solux_util.h"

namespace solux {

// Open-addressed facet counter: one 5-byte slot per hit ord, packing
//   word = ((ord + 1) << countBits) | count       (little-endian, 40 bits)
// The key is ord+1 so a live slot's word is never 0 even when count wraps to 0;
// word == 0 is the empty-slot sentinel (no tombstones, no metadata array).
// count is a plain countBits-wide field; on wrap past 2^countBits the multiple
// goes to a global overflow map. Exclusive per-accumulator use (no atomics).
class FlatSlotCounter {
public:
  static constexpr int W = 5;
  static constexpr int SLOT_BITS = 8 * W;
  static constexpr uint64_t SLOT_MASK = ((uint64_t)1 << SLOT_BITS) - 1;
  static constexpr double MAX_LOAD = 0.7;

  size_t maxOrd;
  int keyBits;
  int countBits;
  uint64_t countMask;
  uint64_t countCap;
  std::vector<uint8_t> slots;
  size_t capacity;
  size_t live;
  boost::unordered_flat_map<uint64_t, uint64_t> overflow;

  static inline uint64_t mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  explicit FlatSlotCounter(size_t maxOrd) : maxOrd(maxOrd) {
    static_assert(std::endian::native == std::endian::little,
                  "FlatSlotCounter assumes little-endian slot packing");
    keyBits = maxOrd > 0 ? (int)std::bit_width(maxOrd) : 1;
    assert(keyBits >= 1 && keyBits <= SLOT_BITS - 1);
    countBits = SLOT_BITS - keyBits;
    countMask = ((uint64_t)1 << countBits) - 1;
    countCap = (uint64_t)1 << countBits;
    capacity = 64;
    live = 0;
    allocSlots(capacity);
  }

  void allocSlots(size_t cap) {
    // Every access uses an unaligned 8-byte window. The zeroed pad covers the
    // three bytes beyond the last 5-byte slot.
    slots.assign(cap * W + 8, 0);
  }

  uint64_t SOLUX_INLINE readWord(size_t idx) const {
    uint64_t w;
    std::memcpy(&w, slots.data() + idx * W, 8);
    return w & SLOT_MASK;
  }

  void SOLUX_INLINE writeWord(size_t idx, uint64_t val) {
    uint8_t* p = slots.data() + idx * W;
    uint64_t w;
    std::memcpy(&w, p, 8);
    // Preserve the low three bytes of the neighboring slot touched by the
    // unaligned RMW window.
    w = (w & ~SLOT_MASK) | (val & SLOT_MASK);
    std::memcpy(p, &w, 8);
  }

  size_t SOLUX_INLINE findSlot(int64_t ord) const {
    assert(ord >= 0 && (size_t)ord < maxOrd);
    uint64_t key = (uint64_t)ord + 1;
    size_t mask = capacity - 1;
    size_t idx = (size_t)mix((uint64_t)ord) & mask;
    for (;;) {
      uint64_t w = readWord(idx);
      if (w == 0 || (w >> countBits) == key) {
        return idx;
      }
      idx = (idx + 1) & mask;
    }
  }

  void SOLUX_INLINE increment(int64_t ord) {
    increment(ord, 1);
  }

  void SOLUX_INLINE increment(int64_t ord, int64_t val) {
    assert(ord >= 0 && (size_t)ord < maxOrd && val >= 0);
    uint64_t key = (uint64_t)ord + 1;
    size_t mask = capacity - 1;
    size_t idx = (size_t)mix((uint64_t)ord) & mask;
    for (;;) {
      uint64_t w = readWord(idx);
      if (w == 0) {
        uint64_t total = (uint64_t)val;
        uint64_t newCount = total & countMask;
        uint64_t carry = total & ~countMask;
        if (carry) {
          overflow[(uint64_t)ord] += carry;
        }
        // The ord+1 key keeps this nonzero even when newCount is zero.
        writeWord(idx, (key << countBits) | newCount);
        live++;
        if (live > (size_t)(capacity * MAX_LOAD)) {
          grow();
        }
        return;
      }
      if ((w >> countBits) == key) {
        uint64_t total = (w & countMask) + (uint64_t)val;
        uint64_t newCount = total & countMask;
        uint64_t carry = total & ~countMask;
        if (carry) {
          overflow[(uint64_t)ord] += carry;
        }
        writeWord(idx, (key << countBits) | newCount);
        return;
      }
      idx = (idx + 1) & mask;
    }
  }

  int64_t total(int64_t ord) const {
    size_t idx = findSlot(ord);
    uint64_t w = readWord(idx);
    int64_t t = w == 0 ? 0 : (int64_t)(w & countMask);
    auto it = overflow.find((uint64_t)ord);
    if (it != overflow.end()) {
      t += (int64_t)it->second;
    }
    return t;
  }

  size_t distinct() const {
    return live;
  }

  void grow() {
    size_t newCap = capacity * 2;
    std::vector<uint8_t> old;
    old.swap(slots);
    size_t oldCap = capacity;
    capacity = newCap;
    allocSlots(newCap);
    size_t mask = newCap - 1;
    for (size_t i = 0; i < oldCap; i++) {
      uint64_t w;
      std::memcpy(&w, old.data() + i * W, 8);
      w &= SLOT_MASK;
      if (w == 0) {
        continue;
      }
      int64_t ord = (int64_t)(w >> countBits) - 1;
      size_t idx = (size_t)mix((uint64_t)ord) & mask;
      while (readWord(idx) != 0) {
        idx = (idx + 1) & mask;
      }
      // Re-place the raw word. Counts and the separate overflow map are
      // unchanged by growth.
      writeWord(idx, w);
    }
  }

  void SOLUX_NOINLINE merge(const FlatSlotCounter& other) {
    assert(other.maxOrd == maxOrd);
    for (size_t i = 0; i < other.capacity; i++) {
      uint64_t w = other.readWord(i);
      if (w == 0) {
        continue;
      }
      int64_t ord = (int64_t)(w >> countBits) - 1;
      increment(ord, (int64_t)(w & countMask));
    }
    // The slot holds only the low bits. Add the other's overflow multiple
    // separately so every part of its true total is folded exactly once.
    for (const auto& [ord, cnt] : other.overflow) {
      overflow[ord] += cnt;
    }
  }

  template<typename Accept>
  void foldOverflow(Accept&& accept) {
    for (auto& [ord, ovf] : overflow) {
      size_t idx = findSlot((int64_t)ord);
      uint64_t w = readWord(idx);
      assert(w != 0);
      int64_t total = (int64_t)ovf + (int64_t)(w & countMask);
      // Keep the ord+1 key, so the live slot remains distinguishable from the
      // word==0 empty sentinel after its low count bits are cleared.
      writeWord(idx, ((ord + 1) << countBits));
      accept((int64_t)ord, total);
    }
  }

  template<typename Accept>
  void forEachCount(Accept&& accept) const {
    for (size_t i = 0; i < capacity; i++) {
      uint64_t w = readWord(i);
      if (w != 0) {
        accept((int64_t)(w >> countBits) - 1, (int64_t)(w & countMask));
      }
    }
  }

  void releaseStorage() {
    slots.clear();
    slots.shrink_to_fit();
  }

  size_t bytesUsed() const {
    return slots.capacity()
         + overflow.bucket_count() * (sizeof(std::pair<uint64_t, uint64_t>) + 1);
  }
};

}
