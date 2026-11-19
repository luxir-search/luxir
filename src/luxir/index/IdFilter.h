// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/store/Directory.h"

namespace luxir {

// A segment's id membership filter (s<seg>__idfilter_00_00): a split block
// Bloom filter over the fitted id terms of its dictionary, so that applying
// deletes skips a segment that cannot hold an id without reading its id
// dictionary. A NO answer is a proof; MAYBE (about 1% of absent ids) is not.
// Ids of deleted documents stay in. It is a segment overlay of kind
// "id_filter" at generation 0, written with the segment like its summary
// sidecar; a segment without one is looked up in its dictionary.
//
// Blocks are 256 bits (eight 32-bit words). A key's xxh3 picks a block with
// its high 32 bits and sets one bit per word with its low 32 bits times a
// word's salt (the Parquet SBBF scheme), so a probe reads one cache line.
//
// File layout, little-endian:
//   [0, 8)    MAGIC
//   [8, 12)   u32 FORMAT_VERSION
//   [12, 16)  u32 reserved, 0
//   [16, 24)  u64 keys added
//   [24, 32)  u64 block count
//   [32, 32 + 32 * blocks)  the blocks
//   then      u64 xxh3 of every byte before it
class IdFilter {
public:
  static constexpr std::string_view OVERLAY_KIND = "id_filter";
  static constexpr std::string_view OVERLAY_NAME = "idfilter";
  static constexpr uint64_t OVERLAY_GEN = 0;
  static constexpr std::string_view MAGIC = "LUXIRIDF";
  static constexpr uint32_t FORMAT_VERSION = 1;
  static constexpr size_t HEADER_BYTES = 32;
  static constexpr size_t BLOCK_WORDS = 8;
  static constexpr size_t BLOCK_BYTES = BLOCK_WORDS * sizeof(uint32_t);
  // About 1% false positives.
  static constexpr uint64_t BITS_PER_KEY = 10;

  static std::string overlayFileName(uint64_t segId, uint64_t creatorEpoch, uint64_t writerEpoch);
  static uint64_t hash(std::string_view term) noexcept;

  // Collects the keys of one segment. Sized up front for an upper bound on the
  // keys; more keys only raise the false positive rate. add() may run on
  // several threads (partitioned merges write one field's runs in parallel).
  class Builder {
    std::unique_ptr<std::atomic<uint32_t>[]> words;
    uint64_t blocks;
    std::atomic<uint64_t> keys{0};

  public:
    explicit Builder(uint64_t expectedKeys);
    void add(std::string_view term) noexcept;
    uint64_t numKeys() const noexcept { return keys.load(std::memory_order_relaxed); }
    std::vector<std::byte> encode() const;
    FileDescriptor write(Directory& dir, std::string_view fileName) const;
  };

  // Null when the file does not exist; throws std::runtime_error when it is
  // present but not a readable filter.
  static std::shared_ptr<const IdFilter> open(Directory& dir, std::string_view fileName);
  static std::shared_ptr<const IdFilter> fromBytes(std::span<const std::byte> image);

  bool mayContain(std::string_view term) const noexcept { return mayContainHash(hash(term)); }
  bool mayContainHash(uint64_t h) const noexcept;
  uint64_t numKeys() const noexcept { return keys; }
  size_t bytes() const noexcept { return HEADER_BYTES + words.size() * sizeof(uint32_t) + sizeof(uint64_t); }

private:
  std::vector<uint32_t> words;
  uint64_t blocks = 0;
  uint64_t keys = 0;
};

} // namespace luxir
