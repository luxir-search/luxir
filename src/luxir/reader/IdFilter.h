// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace luxir {

// A membership filter over the terms of a segment's root id dictionary, stored
// as the id field's FILTER part. A miss proves the segment holds no document
// with that id. Ids of deleted documents stay in.
//
// Keys are the XXH3 of the term bytes exactly as the dictionary stores them (the
// fitted form), the same hash whose low byte the term blocks keep. The record's
// term count is the key count.
//
// The part names its own kind and parameters, so the writer may choose them
// per segment and a reader needs nothing else. A part that does not parse is
// damaged. It starts with a HEADER_BYTES header:
//   [0]       u8  kind (Kind)
//   [1]       u8  BLOOM: bits per key the writer asked for (informational)
//                 FUSE: fingerprint bits, 8 or 16
//   [2, 4)    zero
//   [4, 8)    u32 FUSE: segment length, a power of two; BLOOM: zero
//   [8, 12)   u32 FUSE: segment count; BLOOM: zero
//   [12, 16)  zero
//   [16, 24)  u64 FUSE: seed; BLOOM: zero
// and the body follows.
//
// BLOOM is a split-block Bloom filter (the Parquet scheme). Each block is eight
// 32-bit words (32 bytes). A key's high 32 bits pick a block and its low 32 bits
// set one bit per word through a per-word salt, so a probe reads one block. Its
// false positive rate falls with bits per key (about 1.3% at 10, 0.13% at 16).
// It is built in one pass with only an upper bound on the key count.
//
// FUSE is a 3-wise binary fuse filter (Graf and Lemire): (segment count + 2) *
// segment length fingerprints of 8 or 16 bits. A probe reads three
// fingerprints. Its false positive rate is 2^-bits (0.39% or 0.0015%) at 1.13
// (millions of keys) to about 1.25 (thousands) fingerprints per key. It is
// static: the builder holds every key until the dictionary ends.
class IdFilter {
public:
  enum class Kind : uint8_t { NONE = 0, BLOOM = 1, FUSE = 2 };

  // What a writer builds. Writers build no filter unless configured to.
  struct Options {
    Kind kind = Kind::NONE;
    uint32_t bitsPerKey = 16;  // BLOOM
    // BLOOM: smallest body. The part is read whole, so small segments get a
    // lower false positive rate for RAM only.
    uint32_t minBytes = 16 * 1024;
    uint32_t fingerprintBits = 16;  // FUSE: 8 or 16
  };
  static constexpr uint32_t MAX_BITS_PER_KEY = 64;

  static constexpr size_t HEADER_BYTES = 24;
  static constexpr size_t BLOCK_WORDS = 8;
  static constexpr size_t BLOCK_BYTES = BLOCK_WORDS * sizeof(uint32_t);
  // The block index is the high half of a 32 by 32 bit product.
  static constexpr uint64_t MAX_BLOCKS = UINT32_MAX;
  static constexpr uint32_t FUSE_MAX_SEGMENT_LENGTH = 1u << 18;

private:
  const char* body = nullptr;
  uint64_t bodyBytes = 0;
  Kind kind = Kind::NONE;
  uint32_t fingerprintBits = 0;
  uint32_t segmentLength = 0;
  uint32_t segmentCount = 0;
  uint64_t seed = 0;

public:
  static uint64_t hash(std::string_view term) noexcept;
  // The filter in `bytes`, or none when they are not a valid filter.
  static std::optional<IdFilter> parse(std::span<const char> bytes) noexcept;
  static bool valid(std::span<const char> bytes) noexcept { return parse(bytes).has_value(); }
  // BLOOM body blocks for `keys` keys under `options`.
  static uint64_t bloomBlocksFor(const Options& options, uint64_t keys) noexcept;
  // Most memory a Builder over at most `keys` keys holds, finish() included.
  static uint64_t buildBytes(const Options& options, uint64_t keys) noexcept;

  Kind filterKind() const noexcept { return kind; }
  // Fingerprint bits of a FUSE filter, zero for BLOOM.
  uint32_t fuseFingerprintBits() const noexcept { return fingerprintBits; }
  uint64_t sizeInBytes() const noexcept { return HEADER_BYTES + bodyBytes; }

  bool mayContain(std::string_view term) const noexcept { return mayContainHash(hash(term)); }
  bool mayContainHash(uint64_t hash) const noexcept;

  // Collects one dictionary's keys. Sized for an upper bound on the keys;
  // fewer keys only lower a BLOOM filter's false positive rate.
  class Builder {
    Options options;
    std::vector<uint32_t> words;  // BLOOM blocks, BLOCK_WORDS per block.
    std::vector<uint64_t> keys;  // FUSE keys, until finish().
    uint64_t added = 0;

  public:
    // options.kind is BLOOM or FUSE.
    Builder(const Options& options, uint64_t maxKeys);
    void add(uint64_t hash);
    uint64_t numKeys() const noexcept { return added; }
    // Memory held between keys.
    size_t sizeInBytes() const noexcept {
      return words.capacity() * sizeof(uint32_t) + keys.capacity() * sizeof(uint64_t);
    }
    // Extra memory finish() holds while it builds.
    size_t finishBytes() const noexcept;
    // The encoded part. A FUSE filter whose construction fails falls back to BLOOM.
    std::vector<char> finish();
  };
};

} // namespace luxir
