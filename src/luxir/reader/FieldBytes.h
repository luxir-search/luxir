// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "luxir/schema/FieldType.h"

namespace luxir {

class PostingsReader;

// Bytes one segment spends on each field, by structure. Computed at read time
// from the field directory: every structure start a SegFieldInfo records,
// plus the extents the readers can size exactly (norms, docs-with-field
// bitsets, the docs and positions of a partitioned term merge). A structure
// with only a recorded start owns the bytes up to the next boundary in its
// file, so trailing alignment padding is charged to the structure before it.
// Bytes no field owns (file headers, the field directory, segment info, gaps
// after exact extents) are `unattributed`. Reads metadata only.
struct FieldBytes {
  enum Kind : uint8_t {
    TERMS,            // term blocks, with per-term metadata and impact frontiers
    TERM_INDEX,       // term trie, block offset table and range table
    POSTINGS,         // doc ids, freqs and block skip/impact headers
    POSITIONS,
    NORMS,
    DOCS_WITH_FIELD,  // presence bitset (absent when every doc has a value)
    COLUMN,           // int, ord or str column values and their metadata
    COLUMN_INDEX,     // monotonic rank, offset and value-to-doc side columns
    POINTS,           // 1-D points leaves and inner nodes
    STORED,           // stored-fields resource chunks and offsets
    VECTORS,          // vector column rows
    NUM_KINDS
  };

  std::string field;
  FieldType::Type type = FieldType::NONE;
  std::array<uint64_t, NUM_KINDS> bytes{};
  // Header counts; deleted docs included.
  uint64_t docs = 0;       // docs with a value
  uint64_t values = 0;     // column values
  uint64_t terms = 0;
  uint64_t postings = 0;   // sum of doc freqs
  uint64_t positions = 0;  // sum of term freqs, for fields with positions

  uint64_t total() const noexcept {
    uint64_t sum = 0;
    for (uint64_t b : bytes) sum += b;
    return sum;
  }

  static const char* typeName(FieldType::Type type) noexcept;
};

struct SegmentFieldBytes {
  std::vector<FieldBytes> fields;  // field directory order (sorted by name)
  uint64_t unattributed = 0;
};

SegmentFieldBytes segmentFieldBytes(PostingsReader& reader);

} // namespace luxir
