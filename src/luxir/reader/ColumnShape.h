// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <optional>

#include "DocsReader.h"
#include "MonoReader.h"
#include "PostingsReader.h"

namespace luxir {

// Which documents of a segment have values in a column, and the range of value
// ranks each one holds. Int, ord and raw string columns all lay this out the
// same way (docs-with-field plus, when multi-valued, a column of end value
// ranks), so it reads without decoding a value. Multiplicity is physical: ord
// writers omit the end value ranks when no document holds more than one value.
class ColumnShape {
  DocsReader docs;
  std::optional<MonoReader> endValueRanks;

public:
  ColumnShape(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo)
      : docs(postingsReader, fieldInfo) {
    if (fieldInfo.monoLoc.offset() > 0) {
      endValueRanks.emplace(postingsReader, fieldInfo.monoLoc,
                            fieldInfo.monoMetaOff, fieldInfo.docsWithField);
    }
  }

  bool multiValued() const noexcept { return endValueRanks.has_value(); }

  // Whether every document of the segment holds exactly one value.
  bool oneValuePerDoc() const noexcept {
    return !endValueRanks && !docs.hasBitset() && docs.numDocs() > 0;
  }

  // Values held by all documents of the segment.
  int64_t numValues() const {
    if (!endValueRanks) return docs.numDocs();
    return docs.numDocs() == 0 ? 0 : endValueRanks->valueAt(docs.numDocs() - 1);
  }

  // Visits the value rank range [first, end) of every document holding a
  // value, in document order, decoding the end value ranks a frame at a time.
  template <typename Visit>
  void forEachDocValues(Visit&& visit) const {
    int32_t numDocs = docs.numDocs();
    if (!endValueRanks) {
      for (int32_t rank = 0; rank < numDocs; rank++) visit((int64_t)rank, (int64_t)rank + 1);
      return;
    }
    std::array<int64_t, 128> ends;
    int64_t first = 0;
    for (int32_t rank = 0; rank < numDocs; rank += (int32_t)ends.size()) {
      size_t count = std::min(ends.size(), (size_t)(numDocs - rank));
      endValueRanks->readValues(rank, std::span<int64_t>(ends.data(), count));
      for (size_t i = 0; i < count; i++) {
        visit(first, ends[i]);
        first = ends[i];
      }
    }
  }

  // Seeks documents in any order: repeating a target is free, and a target
  // behind the previous one restarts the scan.
  class Cursor {
    const ColumnShape* shape;
    screaming::BitSet::Iterator docsIter;
    int32_t found = -1;
    int32_t target = -1;
    int32_t docRank = -1;
    int64_t first = 0;
    int64_t last = 0;
    bool present = false;

  public:
    explicit Cursor(const ColumnShape& shape)
        : shape(&shape), docsIter(shape.docs.bitset()) {}

    // Whether `docid` holds a value; when it does, rank(), begin() and end()
    // describe it.
    bool seek(int32_t docid) {
      if (docid == target) return present;
      if (docid < target) {
        docsIter.reset();
        found = -1;
      }
      target = docid;
      if (!shape->docs.hasBitset()) {
        // No bitset: every document, or none when a merge emptied the column.
        present = docid < shape->docs.numDocs();
        if (!present) return false;
        docRank = docid;
      } else {
        if (found < docid) found = docsIter.advance(docid);
        present = found == docid;
        if (!present) return false;
        docRank = docsIter.rank();
      }
      present = true;
      if (shape->endValueRanks) {
        auto [start, end] = shape->endValueRanks->valuesAt(docRank);
        first = start;
        last = end;
      } else {
        first = docRank;
        last = docRank + 1;
      }
      return true;
    }

    // Values held by all rows in [first, end), including sparse columns.
    // A boundary is the start rank of the next row with a value.
    std::pair<int64_t, int64_t> range(int32_t first, int32_t end) {
      if (first >= end) return {0, 0};
      if (!shape->docs.hasBitset() && !shape->endValueRanks) {
        return {std::min(first, shape->docs.numDocs()), std::min(end, shape->docs.numDocs())};
      }
      auto boundary = [&](int32_t row) -> int64_t {
        if (seek(row)) return begin();
        int32_t rank = !shape->docs.hasBitset() || found == PostingsReader::END
            ? shape->docs.numDocs() : docsIter.rank();
        if (!shape->endValueRanks) return rank;
        return rank == 0 ? 0 : shape->endValueRanks->valueAt(rank - 1);
      };
      int64_t begin = boundary(first);
      return {begin, boundary(end)};
    }

    int32_t rank() const noexcept { return docRank; }
    int64_t begin() const noexcept { return first; }
    int64_t end() const noexcept { return last; }
    int64_t count() const noexcept { return last - first; }
  };
};

} // namespace luxir
