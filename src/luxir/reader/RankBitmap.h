// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/invlists/InvertedLists.h>

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace luxir {

/// Rank-space eligibility for one segment's vector field: bit r is set iff
/// vector rank r may be returned.  Either liveness alone (cached per liveDocs
/// generation) or a filter domain with liveness folded in (cached per filter
/// key and liveDocs generation, or built per query when the filter has no
/// stable key).  LSB-first byte layout, which is what faiss::IDSelectorBitmap
/// consumes; bits past the vector count in the final byte are zero, so
/// popcount(bits) == count.  `bits` points either into bytes owned by
/// `backing` or directly at the segment's liveDocs words (dense
/// single-valued fields: valueRank == docId, and the little-endian byte view
/// of the words IS this layout); either way `backing` pins the storage for as
/// long as a query holds the bitmap.
struct RankBitmap {
  const uint8_t* bits = nullptr;
  size_t numBytes = 0;
  // Set ranks: the segment's eligible vector count.
  int64_t count = 0;
  // Documents owning a set rank, or -1 when not counted (count bounds it).
  int64_t docs = -1;
  std::shared_ptr<const void> backing;
  // Eligible members per IVF list of the segment's overlay, counted on first
  // use (-1 = not counted yet) and kept with the bitmap, so a cached bitmap
  // prices and skips lists without rereading their ids.  Every thread that
  // counts a list stores the same value, so relaxed races are benign.  Empty
  // when the segment has no overlay.
  std::unique_ptr<std::atomic<int64_t>[]> listCounts;
  int32_t nlist = 0;

  void enableListCounts(int32_t lists) {
    nlist = lists;
    listCounts = std::make_unique<std::atomic<int64_t>[]>((size_t)lists);
    for (int32_t i = 0; i < lists; i++) listCounts[(size_t)i].store(-1, std::memory_order_relaxed);
  }

  bool test(int64_t rank) const {
    return ((bits[(size_t)(rank >> 3)] >> (rank & 7)) & 1) != 0;
  }

  /// Eligible members of one IVF list.  Ids out of range are not counted,
  /// mirroring IDSelectorBitmap's byte-bound guard.
  int64_t listCount(const faiss::InvertedLists& lists, int64_t listId) const {
    bool cached = listCounts != nullptr && listId >= 0 && listId < nlist;
    if (cached) {
      int64_t known = listCounts[(size_t)listId].load(std::memory_order_relaxed);
      if (known >= 0) return known;
    }
    size_t size = lists.list_size((size_t)listId);
    int64_t eligible = 0;
    if (size > 0) {
      const faiss::idx_t* ids = lists.get_ids((size_t)listId);
      for (size_t i = 0; i < size; i++) {
        uint64_t r = (uint64_t)ids[i];
        if ((r >> 3) < numBytes) eligible += (bits[r >> 3] >> (r & 7)) & 1;
      }
      lists.release_ids((size_t)listId, ids);
    }
    if (cached) listCounts[(size_t)listId].store(eligible, std::memory_order_relaxed);
    return eligible;
  }

  size_t ramBytesUsed() const {
    return sizeof(*this) + numBytes + (size_t)nlist * sizeof(std::atomic<int64_t>);
  }
};

namespace rankbits {

/// 64 ranks starting at 64 * word from an LSB-first byte bitmap of numBytes
/// bytes; bytes past the end read as zero.  One unaligned load away from the
/// tail, so word scans need no padded storage.
inline uint64_t loadWord(const uint8_t* bytes, size_t numBytes, size_t word) {
  static_assert(std::endian::native == std::endian::little,
                "rank bitmaps are LSB-first bytes read as little-endian words");
  size_t at = word * 8;
  uint64_t value = 0;
  if (at + 8 <= numBytes) {
    std::memcpy(&value, bytes + at, 8);
  } else if (at < numBytes) {
    std::memcpy(&value, bytes + at, numBytes - at);
  }
  return value;
}

/// Bytes for n ranks, all zero, in word storage (so builders can set whole
/// words); the byte view is the first ceil(n / 8) bytes.
inline std::shared_ptr<std::vector<uint64_t>> zeroWords(int64_t n) {
  return std::make_shared<std::vector<uint64_t>>(((size_t)n + 63) / 64, 0);
}

inline void setRange(uint64_t* words, int64_t begin, int64_t end) {
  if (begin >= end) return;
  size_t first = (size_t)(begin >> 6), last = (size_t)((end - 1) >> 6);
  uint64_t head = ~0ULL << (begin & 63);
  uint64_t tail = ~0ULL >> (63 - ((end - 1) & 63));
  if (first == last) {
    words[first] |= head & tail;
    return;
  }
  words[first] |= head;
  for (size_t w = first + 1; w < last; w++) words[w] = ~0ULL;
  words[last] |= tail;
}

inline int64_t popcount(const uint64_t* words, size_t numWords) {
  int64_t count = 0;
  for (size_t w = 0; w < numWords; w++) count += std::popcount(words[w]);
  return count;
}

/// First set rank at or after `from`, or n when none.  Skips all-zero words
/// whole.
inline int64_t nextSet(const uint8_t* bytes, size_t numBytes, int64_t n, int64_t from) {
  if (from >= n) return n;
  size_t word = (size_t)(from >> 6);
  uint64_t bits = loadWord(bytes, numBytes, word) & (~0ULL << (from & 63));
  size_t numWords = ((size_t)n + 63) / 64;
  while (bits == 0) {
    if (++word >= numWords) return n;
    bits = loadWord(bytes, numBytes, word);
  }
  int64_t rank = (int64_t)(word * 64) + std::countr_zero(bits);
  return rank < n ? rank : n;
}

}  // namespace rankbits

}  // namespace luxir
