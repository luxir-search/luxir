// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "luxir/util/MemPool.h"

namespace luxir {

// Which segments of one reader a weight may execute against, by segment
// ordinal. A non-owning view over words that live in a request pool or a
// caller's vector, so weights (pool-allocated, never destroyed) can carry one.
class SegmentMask {
  uint64_t* words = nullptr;
  size_t count = 0;

  static constexpr uint64_t tailMask(size_t n) {
    return n % 64 == 0 ? ~0ULL : (1ULL << (n % 64)) - 1;
  }

public:
  SegmentMask() = default;
  SegmentMask(uint64_t* words, size_t count) : words(words), count(count) {}

  static size_t wordsFor(size_t n) { return (n + 63) / 64; }

  // A mask over `n` segments with every bit `initial`, in `pool`.
  static SegmentMask make(MemPool& pool, size_t n, bool initial) {
    SegmentMask mask(wordsFor(n) == 0 ? nullptr : pool.make_span<uint64_t>(wordsFor(n)).data(), n);
    mask.fill(initial);
    return mask;
  }
  // The same over caller-owned storage, resized as needed.
  static SegmentMask make(std::vector<uint64_t>& storage, size_t n, bool initial) {
    storage.assign(wordsFor(n), 0);
    SegmentMask mask(storage.data(), n);
    mask.fill(initial);
    return mask;
  }

  SegmentMask copy(MemPool& pool) const {
    SegmentMask result = make(pool, count, false);
    if (count > 0) std::memcpy(result.words, words, wordsFor(count) * sizeof(uint64_t));
    return result;
  }
  SegmentMask copy(std::vector<uint64_t>& storage) const {
    SegmentMask result = make(storage, count, false);
    if (count > 0) std::memcpy(result.words, words, wordsFor(count) * sizeof(uint64_t));
    return result;
  }

  void fill(bool value) noexcept {
    size_t n = wordsFor(count);
    for (size_t i = 0; i < n; i++) words[i] = value ? ~0ULL : 0ULL;
    if (value && n > 0) words[n - 1] &= tailMask(count);
  }

  bool valid() const noexcept { return words != nullptr || count == 0; }
  size_t size() const noexcept { return count; }
  bool empty() const noexcept { return count == 0; }

  bool test(size_t ord) const noexcept {
    assert(ord < count);
    return (words[ord / 64] >> (ord % 64)) & 1;
  }
  void set(size_t ord) noexcept {
    assert(ord < count);
    words[ord / 64] |= 1ULL << (ord % 64);
  }
  void clear(size_t ord) noexcept {
    assert(ord < count);
    words[ord / 64] &= ~(1ULL << (ord % 64));
  }

  size_t popcount() const noexcept {
    size_t n = 0;
    for (size_t i = 0; i < wordsFor(count); i++) n += (size_t)std::popcount(words[i]);
    return n;
  }
  bool isAll() const noexcept { return popcount() == count; }
  bool isNone() const noexcept { return popcount() == 0; }

  SegmentMask& operator&=(const SegmentMask& other) noexcept {
    assert(other.count == count);
    for (size_t i = 0; i < wordsFor(count); i++) words[i] &= other.words[i];
    return *this;
  }
  SegmentMask& operator|=(const SegmentMask& other) noexcept {
    assert(other.count == count);
    for (size_t i = 0; i < wordsFor(count); i++) words[i] |= other.words[i];
    return *this;
  }
  // this & ~other
  SegmentMask& subtract(const SegmentMask& other) noexcept {
    assert(other.count == count);
    for (size_t i = 0; i < wordsFor(count); i++) words[i] &= ~other.words[i];
    return *this;
  }
  // Every ordinal set here is set in `other`.
  bool subsetOf(const SegmentMask& other) const noexcept {
    assert(other.count == count);
    for (size_t i = 0; i < wordsFor(count); i++) {
      if ((words[i] & ~other.words[i]) != 0) return false;
    }
    return true;
  }

  bool equals(const SegmentMask& other) const noexcept {
    if (other.count != count) return false;
    for (size_t i = 0; i < wordsFor(count); i++) {
      if (words[i] != other.words[i]) return false;
    }
    return true;
  }
  bool operator==(const SegmentMask& other) const noexcept { return equals(other); }

  uint64_t hash() const noexcept {
    uint64_t h = count * 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < wordsFor(count); i++) {
      h ^= words[i] + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    }
    return h;
  }

  // Visits every set ordinal in increasing order.
  template <typename F>
  void forEach(F&& f) const {
    for (size_t i = 0; i < wordsFor(count); i++) {
      uint64_t w = words[i];
      while (w) {
        size_t bit = (size_t)std::countr_zero(w);
        f(i * 64 + bit);
        w &= w - 1;
      }
    }
  }
};

} // namespace luxir
