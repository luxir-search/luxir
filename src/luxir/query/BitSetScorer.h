// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "Query.h"
#include "luxir/util/screaming.h"

namespace luxir {

// Constant-scoring scorer over one serialized screaming bitset in a segment:
// a column's docs-with-field set (ExistsQuery) or a BOOLEAN column's true or
// false set (BoolValueQuery). Cursor access iterates the set; window fills
// OR dense buckets in whole words and sparse buckets value by value, so a
// filter over the set costs bitset work, not per-document decoding.
class BitSetScorer : public Query::ConstantScorer {
  screaming::BitSet bits;
  screaming::BitSet::Iterator iterator;

  using Descriptor = screaming::BitSet::BucketDescriptor;
  static constexpr int32_t BUCKET_BITS = screaming::BitSet::BUCKET_BITS;
  static constexpr int32_t BUCKET_SIZE = (int32_t)screaming::BitSet::BUCKET_SIZE;

  // OR the set's members in [lo, hi) into words, bit i standing for doc
  // min + i. min and lo are multiples of 64, so dense bucket words line up
  // with window words.
  void orRange(std::span<uint64_t> words, int32_t min, int32_t lo, int32_t hi) const {
    assert((min & 63) == 0 && (lo & 63) == 0 && min <= lo);
    const Descriptor* begin = bits.descriptors;
    const Descriptor* end = begin + bits.nBuckets;
    // The first bucket that can hold lo.
    const Descriptor* desc = std::lower_bound(
        begin, end, (uint16_t)((uint32_t)lo >> BUCKET_BITS),
        [](const Descriptor& d, uint16_t upperBits) { return d.upperBits < upperBits; });
    for (; desc != end; desc++) {
      int32_t base = (int32_t)((uint32_t)desc->upperBits << BUCKET_BITS);
      if (base >= hi) break;
      int32_t from = std::max(base, lo);
      int32_t to = std::min(base + BUCKET_SIZE, hi);
      int32_t size = (int32_t)desc->size + 1;
      if (size <= (int32_t)screaming::BitSet::BUCKET_SPARSE_MAX) {
        const uint16_t* values = (const uint16_t*)(bits.start + desc->offset);
        const uint16_t* stop = values + size;
        for (const uint16_t* v = std::lower_bound(values, stop, (uint16_t)(from - base));
             v != stop && base + *v < to; v++) {
          int32_t bit = base + *v - min;
          words[(size_t)bit >> 6] |= 1ULL << (bit & 63);
        }
        continue;
      }
      const uint64_t* source = (const uint64_t*)(bits.start + desc->offset);
      for (int32_t doc = from; doc < to; doc += 64) {
        uint64_t word = source[(doc - base) >> 6];
        if (to - doc < 64) word &= (1ULL << (to - doc)) - 1;
        words[(size_t)(doc - min) >> 6] |= word;
      }
    }
  }

public:
  // end points just past the serialized set (SegFieldInfo's *EndLoc).
  BitSetScorer(const void* end, float constantScore)
      : Query::ConstantScorer(constantScore), bits(end), iterator(bits) {
    assert(!bits.empty());
  }

  [[gnu::cold]] void reset() override {
    iterator.reset();
  }

  int32_t next() override {
    return iterator.next();
  }

  int32_t advance(int32_t target) override {
    assert(docId() < target);
    return iterator.advance(target);
  }

  int32_t docId() override { return iterator.val(); }

  void fillScoredWindowBits(std::span<uint64_t> words,
                            int32_t min, int32_t max) override {
    fillWindowBits(words, min, max);
  }

  // Members from the cursor on, as the base iteration reports them; leaves
  // the cursor on the first member at or past max. The cursor's own partial
  // word goes bit by bit, the rest of the window in whole words.
  void fillWindowBits(std::span<uint64_t> words, int32_t min,
                      int32_t max) override {
    if (min >= max) return;
    int32_t doc = iterator.val();
    if (doc < min) doc = iterator.advance(min);
    if (doc >= max) return;
    int32_t aligned = (min & 63) == 0 ? (int32_t)(((int64_t)doc + 63) & ~63LL) : max;
    while (doc < max && doc < aligned) {
      int32_t bit = doc - min;
      words[(size_t)bit >> 6] |= 1ULL << (bit & 63);
      doc = iterator.next();
    }
    if (doc >= max) return;
    orRange(words, min, aligned, max);
    iterator.advance(max);
  }
};

static_assert(std::is_trivially_destructible_v<BitSetScorer>);

} // namespace luxir
