#pragma once

#include <algorithm>
#include <array>
#include <assert.h>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "solux/util/StrRef.h"

namespace solux {

/**
 * Builds the compact trie used to route term seek targets to term blocks.
 *
 * The trie indexes minimal separator keys to dense term-block ordinals.  There
 * is one key for every terms block after the first; block 0 is represented by
 * the root's implicit empty separator.  Keys are compared as unsigned bytes and
 * must be added in strictly increasing order with ordinals 1, 2, 3, ...
 *
 * Construction is a streaming frontier build.  Only the right edge of the trie
 * is live while keys are added.  When a new key diverges from the previous key,
 * the now-complete suffix of the previous key is serialized bottom-up.  Children
 * are therefore written before their parents, and parent child pointers are
 * backward deltas computed when the parent freezes.
 *
 * Serialized node format:
 *
 *   header byte:
 *     bits 0..1: sign (LEAF, SINGLE, MULTI)
 *     bit 2:     terminal separator key ends at this node
 *     bits 3..5: child delta byte width minus 1
 *     bits 6..7: MULTI child-label strategy
 *
 *   LEAF:
 *     [header]
 *
 *   SINGLE:
 *     [header][label][childDelta:dw][childLo:3]
 *
 *   MULTI:
 *     [header][minLabel][strategyBytesMinus1][strategy bytes]
 *     [childDelta:dw][childLo:3] repeated once per present label
 *
 * Child entries are stored in label order.  childLo is the lowest block ordinal
 * in that child's subtree.  It lives in the parent entry because floor lookups
 * often need "the block before this child subtree" before loading the child.
 *
 * MULTI label strategies store only the set of child bytes:
 *   BITS: bitmap from minLabel through maxLabel
 *   ARRAY: all labels after minLabel, sorted ascending
 *   REVERSE_ARRAY: maxLabel followed by sorted labels absent between min/max
 *
 * The builder chooses the strategy with the fewest strategy bytes.  Ties prefer
 * BITS, then ARRAY, then REVERSE_ARRAY.  The finished byte region ends with
 * eight zero bytes so readers can use one unaligned 8-byte load near the end of
 * the trie without reading beyond initialized memory.
 */
class TrieBuilder {
public:
  static constexpr uint8_t SIGN_LEAF = 0;
  static constexpr uint8_t SIGN_SINGLE = 1;
  static constexpr uint8_t SIGN_MULTI = 2;
  static constexpr uint8_t TERMINAL_BIT = 1 << 2;

  static constexpr uint8_t STRATEGY_BITS = 0;
  static constexpr uint8_t STRATEGY_ARRAY = 1;
  static constexpr uint8_t STRATEGY_REVERSE_ARRAY = 2;

  static constexpr uint32_t ORD_WIDTH = 3;
  static constexpr uint32_t MAX_BLOCKS = 0xFFFFFF;

private:
  struct FrontierNode {
    std::vector<uint8_t> childLabels;
    std::vector<uint64_t> childFps;
    std::vector<uint32_t> childLos;
    bool terminal = false;
    uint32_t firstOrd = 0;
  };

  std::vector<char> buf;
  std::array<FrontierNode, PackedTerm::MAX_LEN + 2> frontier;
  std::array<uint8_t, PackedTerm::MAX_LEN> prevKey{};
  uint32_t prevLen = 0;
  uint32_t nBlocks = 1;
  bool hasPrev = false;
  bool done = false;

  static int compareBytes(std::string_view a, const uint8_t* b, uint32_t bLen) {
    uint32_t n = std::min((uint32_t)a.size(), bLen);
    for (uint32_t i = 0; i < n; i++) {
      uint8_t ca = (uint8_t)a[i];
      uint8_t cb = b[i];
      if (ca != cb) return (int)ca - (int)cb;
    }
    return (int)a.size() - (int)bLen;
  }

  static uint32_t commonPrefix(std::string_view a, const uint8_t* b, uint32_t bLen) {
    uint32_t n = std::min((uint32_t)a.size(), bLen);
    uint32_t i = 0;
    while (i < n && (uint8_t)a[i] == b[i]) i++;
    return i;
  }

  static uint32_t deltaWidth(uint64_t value) {
    assert(value > 0);
    uint32_t width = 1;
    while (width < 8 && (value >> (width * 8)) != 0) width++;
    return width;
  }

  static void writeLE(std::vector<char>& out, uint64_t value, uint32_t width) {
    for (uint32_t i = 0; i < width; i++) {
      out.push_back((char)((value >> (i * 8)) & 0xff));
    }
  }

  static void writeOrd(std::vector<char>& out, uint32_t ord) {
    assert(ord < MAX_BLOCKS);
    writeLE(out, ord, ORD_WIDTH);
  }

  static void resetNode(FrontierNode& node, uint32_t firstOrd, bool terminal) {
    node.childLabels.clear();
    node.childFps.clear();
    node.childLos.clear();
    node.terminal = terminal;
    node.firstOrd = firstOrd;
  }

  static void appendChild(FrontierNode& parent, uint8_t label, uint64_t fp, uint32_t lo) {
    assert(parent.childLabels.empty() || parent.childLabels.back() < label);
    parent.childLabels.push_back(label);
    parent.childFps.push_back(fp);
    parent.childLos.push_back(lo);
  }

  static uint8_t chooseStrategy(const std::vector<uint8_t>& labels) {
    uint32_t n = (uint32_t)labels.size();
    uint32_t minLabel = labels.front();
    uint32_t maxLabel = labels.back();
    uint32_t span = maxLabel - minLabel + 1;
    uint32_t bitsBytes = (span + 7) / 8;
    uint32_t arrayBytes = n - 1;
    uint32_t reverseBytes = 1 + span - n;

    if (bitsBytes <= arrayBytes && bitsBytes <= reverseBytes) return STRATEGY_BITS;
    if (arrayBytes <= reverseBytes) return STRATEGY_ARRAY;
    return STRATEGY_REVERSE_ARRAY;
  }

  static std::vector<uint8_t> strategyBytes(const std::vector<uint8_t>& labels, uint8_t strategy) {
    uint8_t minLabel = labels.front();
    uint8_t maxLabel = labels.back();
    std::vector<uint8_t> out;

    if (strategy == STRATEGY_BITS) {
      uint32_t span = (uint32_t)maxLabel - (uint32_t)minLabel + 1;
      out.assign((span + 7) / 8, 0);
      for (uint8_t label : labels) {
        uint32_t idx = (uint32_t)label - (uint32_t)minLabel;
        out[idx >> 3] |= (uint8_t)(1u << (idx & 7));
      }
      return out;
    }

    if (strategy == STRATEGY_ARRAY) {
      out.reserve(labels.size() - 1);
      for (uint32_t i = 1; i < (uint32_t)labels.size(); i++) {
        out.push_back(labels[i]);
      }
      return out;
    }

    assert(strategy == STRATEGY_REVERSE_ARRAY);
    out.push_back(maxLabel);
    uint32_t labelIdx = 1;
    for (uint32_t c = (uint32_t)minLabel + 1; c < (uint32_t)maxLabel; c++) {
      while (labelIdx < (uint32_t)labels.size() && labels[labelIdx] < c) labelIdx++;
      if (labelIdx >= (uint32_t)labels.size() || labels[labelIdx] != c) {
        out.push_back((uint8_t)c);
      }
    }
    return out;
  }

  uint64_t freezeNode(const FrontierNode& node) {
    uint64_t nodeFp = (uint64_t)buf.size();
    uint32_t childCount = (uint32_t)node.childLabels.size();

    if (childCount == 0) {
      assert(node.terminal);
      buf.push_back((char)(SIGN_LEAF | TERMINAL_BIT));
      return nodeFp;
    }

    uint64_t maxDelta = 0;
    for (uint64_t childFp : node.childFps) {
      assert(childFp < nodeFp);
      maxDelta = std::max(maxDelta, nodeFp - childFp);
    }
    uint32_t dw = deltaWidth(maxDelta);
    assert(dw >= 1 && dw <= 8);

    if (childCount == 1) {
      uint8_t header = SIGN_SINGLE | (uint8_t)((dw - 1) << 3);
      if (node.terminal) header |= TERMINAL_BIT;
      buf.push_back((char)header);
      buf.push_back((char)node.childLabels[0]);
      writeLE(buf, nodeFp - node.childFps[0], dw);
      writeOrd(buf, node.childLos[0]);
      return nodeFp;
    }

    uint8_t strategy = chooseStrategy(node.childLabels);
    std::vector<uint8_t> strategyData = strategyBytes(node.childLabels, strategy);
    assert(!strategyData.empty());
    assert(strategyData.size() <= 256);

    uint8_t header = SIGN_MULTI | (uint8_t)((dw - 1) << 3) | (uint8_t)(strategy << 6);
    if (node.terminal) header |= TERMINAL_BIT;
    buf.push_back((char)header);
    buf.push_back((char)node.childLabels.front());
    buf.push_back((char)(strategyData.size() - 1));
    for (uint8_t b : strategyData) {
      buf.push_back((char)b);
    }
    for (uint32_t i = 0; i < childCount; i++) {
      writeLE(buf, nodeFp - node.childFps[i], dw);
      writeOrd(buf, node.childLos[i]);
    }
    return nodeFp;
  }

  void freezeDepth(uint32_t depth) {
    uint64_t nodeFp = freezeNode(frontier[depth]);
    appendChild(frontier[depth - 1], prevKey[depth - 1], nodeFp, frontier[depth].firstOrd);
    resetNode(frontier[depth], 0, false);
  }

public:
  TrieBuilder() {
    reset();
  }

  void reset() {
    buf.clear();
    for (FrontierNode& node : frontier) {
      resetNode(node, 0, false);
    }
    resetNode(frontier[0], 0, true);
    prevLen = 0;
    nBlocks = 1;
    hasPrev = false;
    done = false;
  }

  void add(std::string_view key, uint32_t ord) {
    assert(!done);
    assert(!key.empty());
    assert(key.size() <= PackedTerm::MAX_LEN);
    assert(ord == nBlocks);
    assert(ord < MAX_BLOCKS);
    if (hasPrev) {
      assert(compareBytes(key, prevKey.data(), prevLen) > 0);
    }

    uint32_t cp = hasPrev ? commonPrefix(key, prevKey.data(), prevLen) : 0;
    if (hasPrev) {
      for (uint32_t depth = prevLen; depth > cp; depth--) {
        freezeDepth(depth);
      }
    }

    for (uint32_t depth = cp + 1; depth <= (uint32_t)key.size(); depth++) {
      resetNode(frontier[depth], ord, false);
    }
    frontier[key.size()].terminal = true;

    std::memcpy(prevKey.data(), key.data(), key.size());
    prevLen = (uint32_t)key.size();
    hasPrev = true;
    nBlocks = ord + 1;
  }

  uint64_t finish() {
    assert(!done);
    assert(nBlocks <= MAX_BLOCKS);
    if (hasPrev) {
      for (uint32_t depth = prevLen; depth > 0; depth--) {
        freezeDepth(depth);
      }
    }
    uint64_t rootFp = freezeNode(frontier[0]);
    // See the class comment: readers may use one unaligned 8-byte load at the
    // end of the trie region, so leave initialized zero slack after the root.
    for (int i = 0; i < 8; i++) {
      buf.push_back(0);
    }
    done = true;
    return rootFp;
  }

  const std::vector<char>& bytes() const {
    return buf;
  }

  uint32_t blockCount() const {
    return nBlocks;
  }
};

}
