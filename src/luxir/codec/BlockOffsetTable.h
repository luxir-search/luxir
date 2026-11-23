// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace luxir {

// Start offsets of a field's term blocks, relative to the field's first
// dictionary block, written by TextWriter::finishDictionary for both a
// serially written field and a merge's stitched partitions:
//
//   [anchors: (groups - 1) x u64 little-endian]
//   [relative: blocks x bits, LSB first, then slack to the last word]
//
// Blocks form groups of GROUP.  The first group's anchor is always 0 and is
// not stored; group g > 0 stores the offset of its first block.  Every block
// stores its offset minus its group's anchor in `bits` bits, one width for
// the whole field, kept in the field's catalog record.  A lookup is one
// anchor load and one unaligned 8-byte load, so it touches at most two
// small spans of the table.  The relative section ends with enough slack
// that the 8-byte load of the last block stays inside the table.  A field
// whose offsets are all zero (a single block) has bits 0 and no table bytes.
class BlockOffsetTable {
  static constexpr uint64_t ZEROS[1] = {0};

  const char* anchors = nullptr;  // Anchor of group 1, the first one stored.
  const char* relative = (const char*) ZEROS;
  uint64_t mask = 0;
  uint32_t bits = 0;

public:
  static constexpr uint32_t GROUP = 64;
  static constexpr uint32_t GROUP_SHIFT = 6;
  static constexpr uint32_t MAX_BITS = 32;
  static constexpr size_t ALIGN = 8;  // Table start alignment, when nonempty.
  static_assert(GROUP == 1u << GROUP_SHIFT);

  // Narrowest width that holds every offset relative to its anchor.
  static uint32_t bitsFor(std::span<const uint64_t> offsets) {
    uint64_t widest = 0;
    for (size_t i = 0; i < offsets.size(); i++) {
      uint64_t anchor = offsets[i & ~(size_t) (GROUP - 1)];
      assert(offsets[i] >= anchor && (i == 0 || offsets[i] > offsets[i - 1]));
      widest |= offsets[i] - anchor;
    }
    return (uint32_t) std::bit_width(widest);
  }

  static constexpr uint64_t groups(uint64_t blocks) { return (blocks + GROUP - 1) >> GROUP_SHIFT; }

  static constexpr uint64_t anchorBytes(uint64_t blocks) {
    return blocks == 0 ? 0 : (groups(blocks) - 1) * sizeof(uint64_t);
  }

  // The last block's 8-byte load starts at its first bit's byte.
  static constexpr uint64_t relativeBytes(uint64_t blocks, uint32_t bits) {
    return bits == 0 || blocks == 0 ? 0 : (((blocks - 1) * bits) >> 3) + sizeof(uint64_t);
  }

  static constexpr uint64_t bytes(uint64_t blocks, uint32_t bits) {
    return anchorBytes(blocks) + relativeBytes(blocks, bits);
  }

  static void encode(std::span<const uint64_t> offsets, uint32_t bits, std::vector<char>& out) {
    assert(bits <= MAX_BITS && (offsets.empty() || offsets[0] == 0));
    size_t start = out.size();
    out.resize(start + bytes(offsets.size(), bits));
    char* p = out.data() + start;
    for (size_t g = GROUP; g < offsets.size(); g += GROUP) {
      memcpy(p, &offsets[g], sizeof(uint64_t));
      p += sizeof(uint64_t);
    }
    if (bits == 0) return;
    for (size_t i = 0; i < offsets.size(); i++) {
      uint64_t value = offsets[i] - offsets[i & ~(size_t) (GROUP - 1)];
      assert(std::bit_width(value) <= (int) bits);
      uint64_t bit = (uint64_t) i * bits;
      uint64_t word;
      memcpy(&word, p + (bit >> 3), sizeof(word));
      word |= value << (bit & 7);
      memcpy(p + (bit >> 3), &word, sizeof(word));
    }
  }

  BlockOffsetTable() = default;

  // `table` holds bytes(blocks, bits) readable bytes.
  BlockOffsetTable(const char* table, uint64_t blocks, uint32_t bits)
      : anchors(table),
        relative(bits ? table + anchorBytes(blocks) : (const char*) ZEROS),
        mask(bits ? ~uint64_t{0} >> (64 - bits) : 0), bits(bits) {
    assert(bits <= MAX_BITS);
  }

  // Offset of the block from the field's first dictionary block.
  uint64_t operator[](uint32_t block) const {
    uint32_t group = block >> GROUP_SHIFT;
    uint64_t anchor = 0;
    if (group != 0) memcpy(&anchor, anchors + (size_t) (group - 1) * sizeof(uint64_t), sizeof(anchor));
    uint64_t bit = (uint64_t) block * bits;
    uint64_t word;
    memcpy(&word, relative + (bit >> 3), sizeof(word));
    return anchor + ((word >> (bit & 7)) & mask);
  }
};

} // namespace luxir
