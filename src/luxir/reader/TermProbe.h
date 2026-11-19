// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "PostingsReader.h"
#include "TermsEnum.h"

namespace luxir {

// Exact lookups of known terms in one field of a lazy segment (one whose files
// are fetched on first touch, from an object store) that read only the bytes
// they need instead of the whole files holding the field. The reads come in
// dependent rounds, each one batched fetch per file:
//   1. at construction, the field's block index and trie (one range, they
//      are adjacent) and its range table when it has one;
//   2. fetchBlocks: the term block each target routes to;
//   3. fetchDocs: a found term's postings, unless inlined in its block (a
//      term of one document, which an id usually is).
// Lookups then run through a lazy TermsEnum (enumFor): seek(target) and
// postingsState(), on resident bytes only.
class TermProbe {
  PostingsReader& reader;
  const SegFieldInfo& field;

  // A trie node is at most a few KiB (256 children); the root is the last.
  static constexpr uint64_t TRIE_ROOT_MAX = 4096;

  // Makes the ranges resident, clipped to the file. Blocks until they are.
  void fetch(uint32_t fileNum, std::vector<ByteRange>& ranges);

public:
  // True when lookups through a probe save reads: the dictionary's file is
  // lazy and not resident as a whole.
  static bool saves(PostingsReader& reader, const SegFieldInfo& field);

  // Fetches the block index (round 1).
  TermProbe(PostingsReader& reader, const SegFieldInfo& field);

  TermsEnum enumFor(MemPool& pool) { return TermsEnum(pool, reader, field, true); }

  // Fetches the blocks the targets route to (round 2), with any enum of the
  // field.
  void fetchBlocks(const TermsEnum& terms, std::span<const std::string_view> targets);

  // Fetches the postings of the term `terms` is on, unless inlined (round 3).
  void fetchDocs(TermsEnum& terms);
};

} // namespace luxir
