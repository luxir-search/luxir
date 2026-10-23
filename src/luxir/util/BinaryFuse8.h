// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace luxir {

// Static 3-wise binary fuse filter with 8-bit fingerprints (Graf and Lemire,
// "Binary Fuse Filters: Fast and Smaller Than Xor Filters", 2022). About 9
// bits per key at a 1/256 false positive rate; a lookup touches three bytes
// in three adjacent segments, so a remote probe is one small range read.
// The filter is immutable: it is built once from the complete key set and
// serialized as a header followed by the fingerprint array.
class BinaryFuse8 {
public:
  struct Header {
    uint32_t segmentLength;
    uint32_t segmentLengthMask;
    uint32_t segmentCount;
    uint32_t segmentCountLength;
    uint32_t arrayLength;
    uint32_t padding = 0;
    uint64_t seed;
  };
  static_assert(sizeof(Header) == 32);
  static constexpr size_t HEADER_BYTES = sizeof(Header);
  static constexpr uint32_t MAX_RETRIES = 100;
  static constexpr uint64_t DEFAULT_SEED_COUNTER = 0x726b2b9d438b9d4dULL;

  // Builds a filter over `keys` (sorted and deduplicated in place) and appends
  // header plus fingerprints to `out`. Returns false when no seed within
  // `maxRetries` produced an acyclic construction; `out` is then unchanged.
  static bool build(std::vector<uint64_t>& keys, std::vector<std::byte>& out,
                    uint32_t maxRetries = MAX_RETRIES,
                    uint64_t seedCounter = DEFAULT_SEED_COUNTER);

  // Serialized size for `n` distinct keys, header included.
  static size_t sizeForKeys(size_t n);

  // The header of a serialized filter, or nullopt when `payload` is too short
  // or inconsistent with its own array length.
  static std::optional<Header> header(std::span<const std::byte> payload);

  // Membership test over a serialized filter. A malformed payload answers
  // true (maybe), never false: absence is a proof, presence is not.
  static bool mayContain(std::span<const std::byte> payload, uint64_t key);

  // Byte range [begin, end) of `payload` that mayContain(key) reads, for
  // readers that fetch ranges rather than whole files.
  static std::pair<uint64_t, uint64_t> probeWindow(const Header& header, uint64_t key);

  // Exposed for tests: the three fingerprint slots of a hashed key.
  struct Slots {
    uint32_t h0, h1, h2;
  };
  static Slots slots(const Header& header, uint64_t hash);
  static uint64_t mix(uint64_t key, uint64_t seed);
  static uint8_t fingerprint(uint64_t hash) { return (uint8_t)(hash ^ (hash >> 32)); }

private:
  static Header layout(uint32_t size);
};

} // namespace luxir
