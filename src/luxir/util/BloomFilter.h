// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace luxir {

// Classic Bloom filter over 64-bit keys: k double-hashed bit positions in one
// bit array. Incremental and bounded by a byte budget fixed up front, which
// is why it is the fallback when a term set outgrows the key buffer a static
// filter needs. The bit array can be OR-ed with another built from the same
// Header, which the parallel term merge relies on.
class BloomFilter {
public:
  struct Header {
    uint64_t bits;   // array size in bits, a multiple of 64
    uint64_t seed;
    uint32_t k;
    uint32_t padding = 0;
  };
  static_assert(sizeof(Header) == 24);
  static constexpr size_t HEADER_BYTES = sizeof(Header);
  static constexpr uint32_t MAX_K = 16;

  // The filter layout for at most `bytesBudget` serialized bytes holding
  // about `expectedKeys`; k follows from the resulting bits per key.
  static Header plan(uint64_t bytesBudget, uint64_t expectedKeys, uint64_t seed) {
    Header h{};
    uint64_t words = std::max<uint64_t>(1, (bytesBudget > HEADER_BYTES ? bytesBudget - HEADER_BYTES : 0) / sizeof(uint64_t));
    h.bits = words * 64;
    double bitsPerKey = (double)h.bits / (double)std::max<uint64_t>(1, expectedKeys);
    h.k = (uint32_t)std::clamp(std::lround(bitsPerKey * 0.693), 1L, (long)MAX_K);
    h.seed = seed;
    return h;
  }

  static size_t bytesFor(const Header& h) { return HEADER_BYTES + (size_t)(h.bits / 8); }

  class Builder {
    Header header;
    std::vector<uint64_t> words;
    uint64_t added = 0;

  public:
    explicit Builder(Header plan) : header(plan), words((size_t)(plan.bits / 64)) {}

    const Header& layout() const noexcept { return header; }

    void add(uint64_t key) {
      uint64_t h = mixKey(key, header.seed);
      uint64_t a = h, b = (h >> 32) | 1;
      for (uint32_t i = 0; i < header.k; i++) {
        uint64_t bit = (a + i * b) % header.bits;
        words[bit >> 6] |= (uint64_t)1 << (bit & 63);
      }
      added++;
    }

    // OR in a filter built from the same layout.
    void merge(const Builder& other) {
      if (other.words.size() != words.size()) return;
      for (size_t i = 0; i < words.size(); i++) words[i] |= other.words[i];
      added += other.added;
    }

    uint64_t keys() const { return added; }
    size_t bytes() const { return HEADER_BYTES + words.size() * sizeof(uint64_t); }

    void appendTo(std::vector<std::byte>& out) const {
      size_t base = out.size();
      out.resize(base + bytes());
      std::memcpy(out.data() + base, &header, HEADER_BYTES);
      std::memcpy(out.data() + base + HEADER_BYTES, words.data(), words.size() * sizeof(uint64_t));
    }
  };

  static std::optional<Header> header(std::span<const std::byte> payload) {
    if (payload.size() < HEADER_BYTES) return std::nullopt;
    Header h;
    std::memcpy(&h, payload.data(), HEADER_BYTES);
    if (h.bits == 0 || h.bits % 64 != 0 || h.k == 0 || h.k > MAX_K
        || payload.size() < HEADER_BYTES + h.bits / 8) {
      return std::nullopt;
    }
    return h;
  }

  // A malformed payload answers true (maybe), never false.
  static bool mayContain(std::span<const std::byte> payload, uint64_t key) {
    auto h = header(payload);
    if (!h) return true;
    uint64_t hash = mixKey(key, h->seed);
    uint64_t a = hash, b = (hash >> 32) | 1;
    const std::byte* words = payload.data() + HEADER_BYTES;
    for (uint32_t i = 0; i < h->k; i++) {
      uint64_t bit = (a + i * b) % h->bits;
      uint64_t word;
      std::memcpy(&word, words + (bit >> 6) * sizeof(uint64_t), sizeof(word));
      if ((word & ((uint64_t)1 << (bit & 63))) == 0) return false;
    }
    return true;
  }

  // Byte offsets of the k words mayContain(key) reads, for readers that fetch
  // ranges rather than whole payloads.
  static void probeOffsets(const Header& header, uint64_t key, std::vector<uint64_t>& out) {
    uint64_t hash = mixKey(key, header.seed);
    uint64_t a = hash, b = (hash >> 32) | 1;
    for (uint32_t i = 0; i < header.k; i++) {
      uint64_t bit = (a + i * b) % header.bits;
      out.push_back(HEADER_BYTES + (bit >> 6) * sizeof(uint64_t));
    }
  }

private:
  static uint64_t mixKey(uint64_t key, uint64_t seed) {
    uint64_t h = key ^ seed;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
  }
};

} // namespace luxir
