// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace luxir {

// A split-block Bloom filter over the terms of a segment's root id dictionary,
// stored as the id field's FILTER part. A miss proves the segment holds no
// document with that id; a hit is right about 99% of the time for an absent id.
// Ids of deleted documents stay in.
//
// Keys are the XXH3 of the term bytes exactly as the dictionary stores them (the
// fitted form), the same hash whose low byte the term blocks keep. Each block is
// eight 32-bit words (32 bytes). A key's high 32 bits pick a block and its low 32
// bits set one bit per word through a per-word salt (the Parquet scheme), so a
// probe reads one block. The part is the blocks back to back; the record's term
// count is the key count.
class IdFilter {
  std::span<const char> bytes;  // The blocks, borrowed from the segment.

public:
  static constexpr size_t BLOCK_WORDS = 8;
  static constexpr size_t BLOCK_BYTES = BLOCK_WORDS * sizeof(uint32_t);
  static constexpr uint64_t BITS_PER_KEY = 10;  // About 1% false positives.
  // The block index is the high half of a 32 by 32 bit product.
  static constexpr uint64_t MAX_BLOCKS = UINT32_MAX;

  static uint64_t hash(std::string_view term) noexcept;
  static uint64_t blocksFor(uint64_t keys) noexcept;
  static bool validSize(uint64_t bytes) noexcept {
    return bytes && bytes % BLOCK_BYTES == 0 && bytes / BLOCK_BYTES <= MAX_BLOCKS;
  }

  // The bytes must have a valid size.
  explicit IdFilter(std::span<const char> bytes) noexcept : bytes(bytes) {}

  bool mayContain(std::string_view term) const noexcept { return mayContainHash(hash(term)); }
  bool mayContainHash(uint64_t hash) const noexcept;

  // Collects one dictionary's keys. Sized up front for an upper bound on the
  // keys; fewer keys only lower the false positive rate.
  class Builder {
    std::vector<uint32_t> words;  // The blocks, BLOCK_WORDS per block.
    uint64_t keys = 0;  // Keys added.

  public:
    explicit Builder(uint64_t maxKeys);
    void add(uint64_t hash) noexcept;
    uint64_t numKeys() const noexcept { return keys; }
    size_t sizeInBytes() const noexcept { return words.size() * sizeof(uint32_t); }
    std::span<const char> encoded() const noexcept { return {(const char*)words.data(), sizeInBytes()}; }
  };
};

} // namespace luxir
