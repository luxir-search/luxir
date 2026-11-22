// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IdFilter.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <xxhash.h>

namespace luxir {

namespace {

static_assert(std::endian::native == std::endian::little);

constexpr uint32_t SALT[IdFilter::BLOCK_WORDS] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
                                                  0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

uint64_t blockOf(uint64_t hash, uint64_t blocks) noexcept { return ((hash >> 32) * blocks) >> 32; }

uint32_t bitOf(uint64_t hash, size_t word) noexcept { return 1u << (((uint32_t)hash * SALT[word]) >> 27); }

} // namespace

uint64_t IdFilter::hash(std::string_view term) noexcept { return XXH3_64bits(term.data(), term.size()); }

uint64_t IdFilter::blocksFor(uint64_t keys) noexcept {
  auto bits = std::max<uint64_t>(keys, 1) * BITS_PER_KEY;
  return std::min((bits + BLOCK_BYTES * 8 - 1) / (BLOCK_BYTES * 8), MAX_BLOCKS);
}

bool IdFilter::mayContainHash(uint64_t hash) const noexcept {
  auto blocks = bytes.size() / BLOCK_BYTES;
  const char* block = bytes.data() + blockOf(hash, blocks) * BLOCK_BYTES;
  for (size_t w = 0; w < BLOCK_WORDS; w++) {
    uint32_t word;
    std::memcpy(&word, block + w * sizeof(uint32_t), sizeof(word));
    if (!(word & bitOf(hash, w))) return false;
  }
  return true;
}

IdFilter::Builder::Builder(uint64_t maxKeys) : words(blocksFor(maxKeys) * BLOCK_WORDS) {}

void IdFilter::Builder::add(uint64_t hash) noexcept {
  auto* block = words.data() + blockOf(hash, words.size() / BLOCK_WORDS) * BLOCK_WORDS;
  for (size_t w = 0; w < BLOCK_WORDS; w++) block[w] |= bitOf(hash, w);
  keys++;
}

} // namespace luxir
