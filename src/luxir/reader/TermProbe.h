// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <span>
#include <string_view>
#include <vector>

#include "luxir/store/RangeFetch.h"
#include "FieldParts.h"
#include "TermsEnum.h"

namespace luxir {

class SegmentCore;
class DecodedFieldRecord;

// Exact lookups of known terms in one field of a lazy segment (one whose files
// are fetched on first touch, from an object store) that read only the bytes
// they need instead of the field's whole dictionary. The reads come in
// dependent rounds, each one batched fetch per file:
//   1. at construction, the field's block offsets and trie (one range: they
//      are adjacent);
//   2. fetchBlocks: the term block each target routes to;
//   3. fetchPostings: the postings of the terms found, unless inlined in
//      their block (a term of one document, which an id usually is).
// Lookups then run through a sparse TermsEnum (enumFor), on resident bytes.
class TermProbe final : public TermsEnum::SparseHolds {
  SegmentCore& core;
  PostingsReader& reader;
  const DecodedFieldRecord& field;
  ReadDeadline deadline;
  // What the probe's enums read, released with the probe.
  std::mutex holdsMutex;
  std::map<std::pair<uint64_t, uint64_t>, HeldRange> holds;

public:
  // Ranges made resident together: one fetchRanges per file, all files in
  // flight at once. Overlapping and adjacent ranges are coalesced.
  class Round {
    std::map<uint32_t, std::vector<ByteRange>> byFile;

  public:
    void add(field_parts::Slice slice, ReadClass readClass);
    bool empty() const noexcept { return byFile.empty(); }
    uint64_t bytes() const noexcept;
    // Blocks until every range is resident; throws the first fetch error. A
    // file that cannot fetch ahead (FetchNotResidentError) is not an error:
    // its reads fetch on first touch.
    void fetch(SegmentCore& core, ReadDeadline deadline);
  };

  // True when the field's dictionary sits in a lazy file, so a probe saves
  // reads over a TermsEnum, which holds the whole dictionary.
  static bool saves(SegmentCore& core, const DecodedFieldRecord& field);

  // Fetches the block offsets and trie (round 1).
  TermProbe(PostingsReader& reader, const DecodedFieldRecord& field, ReadDeadline deadline = kNoReadDeadline);

  // A sparse enum over the field; the probe keeps what it reads. Enums on
  // different threads may share one probe.
  TermsEnum enumFor(MemPool& pool) { return TermsEnum(pool, reader, field, *this); }

  std::span<const char> hold(field_parts::Slice slice, ReadClass readClass) override;

  // Fetches the blocks the targets route to (round 2). `terms` is an enum
  // from enumFor; it is not moved.
  void fetchBlocks(TermsEnum& terms, std::span<const std::string_view> targets);

  // Fetches postings slices from TermsEnum::postingsSlice (round 3).
  void fetchPostings(std::span<const field_parts::Slice> postings);
};

} // namespace luxir
