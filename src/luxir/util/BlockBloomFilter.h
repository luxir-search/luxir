// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace luxir {

/// Split-block bloom filter over 64-bit hashes (the Parquet / Impala layout).
/// Each key sets one bit in each of the 8 32-bit words of a single 32-byte
/// block, so a probe touches one cache line.  The high hash half picks the
/// block and the low half, multiplied by 8 odd salts, picks the bits.  At
/// about 10 bits per key the false positive rate is roughly 1%.
///
/// Blocks are stored as little-endian uint32 words with no alignment
/// requirement, so a reader can probe them in place in a mapped file.
struct BlockBloomFilter {
  static constexpr uint32_t WORDS_PER_BLOCK = 8;
  static constexpr uint32_t BLOCK_BYTES = WORDS_PER_BLOCK * sizeof(uint32_t);
  static constexpr int64_t BITS_PER_KEY = 10;

  static constexpr uint32_t SALT[WORDS_PER_BLOCK] = {
      0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
      0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

  std::vector<uint32_t> words;

  static uint32_t numBlocksFor(int64_t numKeys) {
    int64_t bits = std::max<int64_t>(numKeys, 1) * BITS_PER_KEY;
    return (uint32_t)((bits + BLOCK_BYTES * 8 - 1) / (BLOCK_BYTES * 8));
  }

  explicit BlockBloomFilter(int64_t numKeys)
      : words((size_t)numBlocksFor(numKeys) * WORDS_PER_BLOCK, 0) {}

  uint32_t numBlocks() const noexcept { return (uint32_t)(words.size() / WORDS_PER_BLOCK); }
  size_t sizeInBytes() const noexcept { return words.size() * sizeof(uint32_t); }

  static uint32_t blockIndex(uint64_t hash, uint32_t numBlocks) noexcept {
    return (uint32_t)(((hash >> 32) * (uint64_t)numBlocks) >> 32);
  }

  static uint32_t bitMask(uint64_t hash, uint32_t word) noexcept {
    return 1u << (((uint32_t)hash * SALT[word]) >> 27);
  }

  void add(uint64_t hash) noexcept {
    uint32_t* block = words.data() + (size_t)blockIndex(hash, numBlocks()) * WORDS_PER_BLOCK;
    for (uint32_t i = 0; i < WORDS_PER_BLOCK; i++) {
      block[i] |= bitMask(hash, i);
    }
  }

  /// Probe a serialized filter of numBlocks blocks starting at blocks.
  static bool mayContain(const char* blocks, uint32_t numBlocks, uint64_t hash) noexcept {
    const char* block = blocks + (size_t)blockIndex(hash, numBlocks) * BLOCK_BYTES;
    for (uint32_t i = 0; i < WORDS_PER_BLOCK; i++) {
      uint32_t word;
      std::memcpy(&word, block + i * sizeof(uint32_t), sizeof(word));
      if ((word & bitMask(hash, i)) == 0) return false;
    }
    return true;
  }
};

} // namespace luxir
