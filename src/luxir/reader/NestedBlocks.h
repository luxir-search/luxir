// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/util/screaming.h"

namespace luxir {

class PostingsReader;

// Per-segment block-join topology for nested documents. A block is written as
// the hidden child documents of one root immediately followed by the root, and
// segment merges keep that order, so a child's root is the next non-child
// document. Built once from the hidden path postings and shared by every
// snapshot holding the segment. Absent (null) for segments without nested docs.
class NestedBlocks {
public:
  int32_t maxDoc;
  // Bit set for every hidden child document.
  screaming::RAMFixedBitSet children;
  // Child documents of each nested path, by path name.
  std::map<std::string, screaming::RAMFixedBitSet, std::less<>> paths;

  explicit NestedBlocks(int32_t maxDoc) : maxDoc(maxDoc), children(maxDoc) {}

  bool isChild(int32_t doc) const { return children.get(doc); }
  // The root document of a child.
  int32_t rootOf(int32_t child) const {
    int32_t next = children.nextClearBit(child);
    return next;
  }
  // First document of the block whose root is `root` (the root itself when it has no children).
  int32_t blockStart(int32_t root) const {
    int32_t doc = root;
    while (doc > 0 && children.get(doc - 1)) --doc;
    return doc;
  }

  // Name of the hidden indexed field holding each child's nested path.
  static constexpr std::string_view PATH_FIELD = "luxir_nested_s";

  // Builds from the postings of PATH_FIELD; null when the segment has no children.
  static std::shared_ptr<const NestedBlocks> build(PostingsReader& reader);
};

}  // namespace luxir
