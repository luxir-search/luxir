// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

namespace luxir {

// Names of the files that hold a collection's _source documents.  They live
// beside the segment files but belong to no segment name: a merge output
// references the files of its inputs, so they must outlive the segment that
// wrote them, and the prefix sweep that deletes a dead segment's files must
// not reach them.  "v<seq>" is a source file, "t<seq>" a tombstone file, with
// <seq> in the form Postings::getSortableString produces: one base-36 length
// character, then that many plus one base-36 digits without a leading zero.
inline bool sortableNumber(std::string_view s) {
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    return -1;
  };
  if (s.size() < 2) return false;
  int extra = digit(s[0]);
  if (extra < 0 || s.size() != (size_t)extra + 2) return false;
  for (char c : s.substr(1)) {
    if (digit(c) < 0) return false;
  }
  return extra == 0 || s[1] != '0';
}

// A segment's source map, "s<seg>__S<gen>", which only a source fetch reads.
inline bool isSourceMapFile(std::string_view name) {
  auto marker = name.find("__S");
  return name.size() > 1 && name[0] == 's' && marker != std::string_view::npos &&
         sortableNumber(name.substr(1, marker - 1)) && sortableNumber(name.substr(marker + 3));
}

// Files that only the _source feature reads.  Neither a search nor opening a
// segment touches them, so startup verification and the scrubber leave them
// alone: damage costs the documents in them, never the collection.
inline bool isSourceFile(std::string_view name) {
  if (name.size() >= 3 && (name[0] == 'v' || name[0] == 't') && sortableNumber(name.substr(1))) return true;
  return isSourceMapFile(name);
}

}  // namespace luxir
