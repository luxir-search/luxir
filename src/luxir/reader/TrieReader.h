#pragma once

#include <algorithm>
#include <assert.h>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "luxir/index/TrieBuilder.h"

namespace luxir {

/**
 * Reads a TrieBuilder separator trie and returns the term block that may contain
 * a target.  floorBlock(target) is the largest separator key <= target, returned
 * as a dense block ordinal.  All byte comparisons are unsigned.
 *
 * Separator ordinals are dense and assigned in key order, so every subtree
 * covers a contiguous ordinal range.  The floor can be computed as:
 *
 *   lo(smallest separator greater than target) - 1
 *
 * A single descent is enough because that successor is always visible on the
 * search path.  When a byte matches a child and that child has a following
 * sibling, rememberedFloor is updated to childLo(next sibling) - 1.  If a later
 * byte has no child >= target byte, rememberedFloor is the answer.  If the next
 * child label is greater than the target byte, that child's lo gives the first
 * separator greater than target, so childLo - 1 is the answer.  If all target
 * bytes are consumed, a terminal node returns nodeLo; a non-terminal node
 * returns nodeLo - 1 because the target is a prefix before every separator in
 * that subtree.  The root is implicitly terminal with lo 0.
 */
class TrieReader {
  const char* base;
  uint64_t rootOff;
  int32_t nBlocks;

  struct Child {
    uint32_t pos = 0;
    uint8_t label = 0;
    bool found = false;
    bool hasNext = false;
  };

  static uint64_t maskBits(uint32_t n) {
    if (n >= 64) return ~0ull;
    if (n == 0) return 0;
    return (1ull << n) - 1;
  }

  static uint64_t readLE(const char* p, uint32_t width) {
    uint64_t value = 0;
    std::memcpy(&value, p, 8);
    return value & maskBits(width * 8);
  }

  // BITS data is followed immediately by child entries.  The reader loads a
  // whole word for speed, so bits beyond the bitmap length must be masked out
  // or entry bytes could be mistaken for present child labels.
  static uint64_t loadBitmapWord(const char* bitmap, uint32_t strategyBytes, uint32_t byteOffset) {
    uint64_t word = 0;
    std::memcpy(&word, bitmap + byteOffset, 8);
    uint32_t validBits = byteOffset >= strategyBytes ? 0 : std::min(64u, (strategyBytes - byteOffset) * 8);
    return word & maskBits(validBits);
  }

  static uint32_t countBitsBelow(const char* bitmap, uint32_t strategyBytes, uint32_t bit) {
    uint32_t count = 0;
    uint32_t byteOffset = 0;
    while (bit >= 64) {
      count += (uint32_t)std::popcount(loadBitmapWord(bitmap, strategyBytes, byteOffset));
      byteOffset += 8;
      bit -= 64;
    }
    uint64_t word = loadBitmapWord(bitmap, strategyBytes, byteOffset) & maskBits(bit);
    count += (uint32_t)std::popcount(word);
    return count;
  }

  static bool anyBitsAbove(const char* bitmap, uint32_t strategyBytes, uint32_t bit) {
    uint32_t totalBits = strategyBytes * 8;
    if (bit + 1 >= totalBits) return false;

    uint32_t next = bit + 1;
    uint32_t byteOffset = (next / 64) * 8;
    uint32_t bitInWord = next & 63;
    while (byteOffset < strategyBytes) {
      uint64_t word = loadBitmapWord(bitmap, strategyBytes, byteOffset);
      if (bitInWord != 0) {
        word &= ~maskBits(bitInWord);
      }
      if (word != 0) return true;
      byteOffset += 8;
      bitInWord = 0;
    }
    return false;
  }

  static uint32_t dw(uint8_t header) {
    return ((header >> 3) & 7) + 1;
  }

  static const char* entryBase(const char* node, uint8_t header) {
    uint8_t sign = header & 3;
    if (sign == TrieBuilder::SIGN_SINGLE) return node + 2;
    assert(sign == TrieBuilder::SIGN_MULTI);
    uint32_t strategyBytes = (uint8_t)node[2] + 1;
    return node + 3 + strategyBytes;
  }

  static uint64_t childDelta(const char* node, uint8_t header, uint32_t pos) {
    uint32_t deltaWidth = dw(header);
    const char* entry = entryBase(node, header) + pos * (deltaWidth + TrieBuilder::ORD_WIDTH);
    return readLE(entry, deltaWidth);
  }

  static uint32_t childLo(const char* node, uint8_t header, uint32_t pos) {
    uint32_t deltaWidth = dw(header);
    const char* entry = entryBase(node, header) + pos * (deltaWidth + TrieBuilder::ORD_WIDTH) + deltaWidth;
    return (uint32_t)readLE(entry, TrieBuilder::ORD_WIDTH);
  }

  static Child ceilBits(const char* node, uint8_t minLabel, uint8_t b) {
    uint32_t strategyBytes = (uint8_t)node[2] + 1;
    const char* bitmap = node + 3;
    uint32_t idx = (uint32_t)b - (uint32_t)minLabel;
    uint32_t totalBits = strategyBytes * 8;
    if (idx >= totalBits) return {};

    uint32_t byteOffset = (idx / 64) * 8;
    uint32_t wordStartBit = byteOffset * 8;
    uint32_t bitInWord = idx - wordStartBit;
    while (byteOffset < strategyBytes) {
      uint64_t word = loadBitmapWord(bitmap, strategyBytes, byteOffset);
      word &= ~maskBits(bitInWord);
      if (word != 0) {
        uint32_t foundIdx = wordStartBit + (uint32_t)std::countr_zero(word);
        Child child;
        child.found = true;
        child.pos = countBitsBelow(bitmap, strategyBytes, foundIdx);
        child.label = (uint8_t)((uint32_t)minLabel + foundIdx);
        child.hasNext = anyBitsAbove(bitmap, strategyBytes, foundIdx);
        return child;
      }
      byteOffset += 8;
      wordStartBit += 64;
      bitInWord = 0;
    }
    return {};
  }

  static Child ceilArray(const char* node, uint8_t b) {
    uint32_t strategyBytes = (uint8_t)node[2] + 1;
    const char* labels = node + 3;
    uint32_t lo = 0;
    uint32_t hi = strategyBytes;
    while (lo < hi) {
      uint32_t mid = (lo + hi) >> 1;
      if ((uint8_t)labels[mid] < b) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo == strategyBytes) return {};

    Child child;
    child.found = true;
    child.pos = lo + 1;
    child.label = (uint8_t)labels[lo];
    child.hasNext = lo + 1 < strategyBytes;
    return child;
  }

  // REVERSE_ARRAY stores maxLabel, then the sorted labels that are absent from
  // the open interval (minLabel, maxLabel).  maxLabel is always present, which
  // bounds the absent-run walk after the lower_bound over absents.
  static Child ceilReverseArray(const char* node, uint8_t minLabel, uint8_t b) {
    uint32_t strategyBytes = (uint8_t)node[2] + 1;
    const char* strategy = node + 3;
    uint8_t maxLabel = (uint8_t)strategy[0];
    if (b > maxLabel) return {};

    const char* absents = strategy + 1;
    uint32_t absentCount = strategyBytes - 1;
    uint32_t lo = 0;
    uint32_t hi = absentCount;
    while (lo < hi) {
      uint32_t mid = (lo + hi) >> 1;
      if ((uint8_t)absents[mid] < b) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }

    uint32_t c = b;
    uint32_t absentIdx = lo;
    while (absentIdx < absentCount && (uint8_t)absents[absentIdx] == c) {
      c++;
      absentIdx++;
    }
    assert(c <= maxLabel);

    Child child;
    child.found = true;
    // absentIdx is the number of absent labels <= c, so subtract it from the
    // dense span index to get the present-child entry position.
    child.pos = (c - (uint32_t)minLabel) - absentIdx;
    child.label = (uint8_t)c;
    child.hasNext = c < maxLabel;
    return child;
  }

  static Child ceilChild(const char* node, uint8_t header, uint8_t b) {
    uint8_t sign = header & 3;
    if (sign == TrieBuilder::SIGN_LEAF) return {};

    if (sign == TrieBuilder::SIGN_SINGLE) {
      uint8_t label = (uint8_t)node[1];
      if (label < b) return {};
      Child child;
      child.found = true;
      child.pos = 0;
      child.label = label;
      child.hasNext = false;
      return child;
    }

    assert(sign == TrieBuilder::SIGN_MULTI);
    uint8_t minLabel = (uint8_t)node[1];
    if (b <= minLabel) {
      Child child;
      child.found = true;
      child.pos = 0;
      child.label = minLabel;
      child.hasNext = true;
      return child;
    }

    uint8_t strategy = header >> 6;
    if (strategy == TrieBuilder::STRATEGY_BITS) return ceilBits(node, minLabel, b);
    if (strategy == TrieBuilder::STRATEGY_ARRAY) return ceilArray(node, b);
    assert(strategy == TrieBuilder::STRATEGY_REVERSE_ARRAY);
    return ceilReverseArray(node, minLabel, b);
  }

public:
  TrieReader(const char* base, uint64_t rootOff, int32_t nBlocks) : base(base), rootOff(rootOff), nBlocks(nBlocks) {
    assert(base != nullptr);
    assert(nBlocks > 0);
    assert(nBlocks <= (int32_t)TrieBuilder::MAX_BLOCKS);
  }

  int32_t floorBlock(std::string_view target) const {
    const char* node = base + rootOff;
    uint64_t nodeFp = rootOff;
    uint8_t header = (uint8_t)node[0];
    uint32_t nodeLo = 0;
    bool nodeTerminal = true;
    int32_t rememberedFloor = nBlocks - 1;

    assert((header & TrieBuilder::TERMINAL_BIT) != 0);
    for (char ch : target) {
      uint8_t b = (uint8_t)ch;
      Child child = ceilChild(node, header, b);
      if (!child.found) return rememberedFloor;
      if (child.label > b) return (int32_t)childLo(node, header, child.pos) - 1;

      if (child.hasNext) {
        rememberedFloor = (int32_t)childLo(node, header, child.pos + 1) - 1;
      }

      uint32_t lo = childLo(node, header, child.pos);
      uint64_t delta = childDelta(node, header, child.pos);
      assert(delta > 0 && delta <= nodeFp);
      nodeFp -= delta;
      node = base + nodeFp;
      header = (uint8_t)node[0];
      nodeLo = lo;
      nodeTerminal = (header & TrieBuilder::TERMINAL_BIT) != 0;
    }

    return nodeTerminal ? (int32_t)nodeLo : (int32_t)nodeLo - 1;
  }
};

}
