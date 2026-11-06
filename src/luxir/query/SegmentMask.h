// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace luxir {

// A set of segment ordinals of one reader: the segments a query or an op may
// match. A default-constructed mask has no size and means "every segment",
// so an op that never computed one prunes nothing.
class SegmentMask {
  std::vector<uint64_t> words;
  size_t count = 0;

public:
  SegmentMask() = default;
  SegmentMask(size_t n, bool initial) : words((n + 63) / 64, initial ? ~0ULL : 0ULL), count(n) {
    if (initial && n % 64 != 0) words.back() &= (1ULL << (n % 64)) - 1;
  }

  size_t size() const noexcept { return count; }
  // True for the default mask, which prunes nothing.
  bool unrestricted() const noexcept { return count == 0; }

  // Whether segment `ord` may match; every ordinal does on an unrestricted mask.
  bool test(size_t ord) const noexcept {
    if (count == 0) return true;
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
    for (uint64_t w : words) n += (size_t)std::popcount(w);
    return n;
  }
  // Whether some segment is excluded.
  bool prunesAny() const noexcept { return count != 0 && popcount() != count; }
  // Whether every segment is excluded (false for a reader without segments).
  bool none() const noexcept { return count != 0 && popcount() == 0; }

  SegmentMask& operator&=(const SegmentMask& other) {
    if (other.unrestricted()) return *this;
    if (unrestricted()) return *this = other;
    assert(other.count == count);
    for (size_t i = 0; i < words.size(); i++) words[i] &= other.words[i];
    return *this;
  }
  SegmentMask& operator|=(const SegmentMask& other) {
    if (unrestricted()) return *this;
    if (other.unrestricted()) return *this = other;
    assert(other.count == count);
    for (size_t i = 0; i < words.size(); i++) words[i] |= other.words[i];
    return *this;
  }
};

} // namespace luxir
