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
// same way (a docs bitmap for sparse fields and, when multi-valued, end value
// ranks), so it reads without decoding a value. Multiplicity is physical: ord
// writers omit the end value ranks when no document holds more than one value.
class ColumnShape {
  DocsReader docs;
  MonoReader endValueRanks;

public:
  ColumnShape(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : docs(reader, fieldInfo) {
    endValueRanks = MonoReader::openEndValueRanks(reader, fieldInfo);
  }

  bool multiValued() const noexcept { return endValueRanks.has_value(); }

  // Whether every document of the segment holds exactly one value.
  bool oneValuePerDoc() const noexcept {
    return !endValueRanks && docs.allRows();
  }

  // Values held by all documents of the segment.
  int64_t numValues() const {
    if (!endValueRanks) return docs.docsWithField();
    return docs.docsWithField() == 0 ? 0 : endValueRanks.valueAt(docs.docsWithField() - 1);
  }

  // Visits the value rank range [first, end) of every document holding a
  // value, in document order, decoding the end value ranks a frame at a time.
  template <typename Visit>
  void forEachDocValues(Visit&& visit) const {
    int32_t numDocs = docs.docsWithField();
    if (!endValueRanks) {
      for (int32_t rank = 0; rank < numDocs; rank++) visit((int64_t)rank, (int64_t)rank + 1);
      return;
    }
    std::array<int64_t, 128> ends;
    int64_t first = 0;
    for (int32_t rank = 0; rank < numDocs; rank += (int32_t)ends.size()) {
      size_t count = std::min(ends.size(), (size_t)(numDocs - rank));
      endValueRanks.readValues(rank, std::span<int64_t>(ends.data(), count));
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
        : shape(&shape), docsIter(shape.docs.bitmapIterator()) {}

    // Whether `docid` holds a value; when it does, rank(), begin() and end()
    // describe it.
    bool seek(int32_t docid) {
      if (docid == target) return present;
      if (docid < target) {
        docsIter.reset();
        found = -1;
      }
      target = docid;
      if (!shape->docs.bitset()) {
        // Every doc has a value when there is no docs bitmap.
        present = docid < shape->docs.docsWithField();
        if (!present) return false;
        docRank = docid;
      } else {
        if (found < docid) found = docsIter.advance(docid);
        present = found == docid;
        if (!present) return false;
        docRank = docsIter.rank();
      }
      if (shape->endValueRanks) {
        auto [start, end] = shape->endValueRanks.valuesAt(docRank);
        first = start;
        last = end;
      } else {
        first = docRank;
        last = docRank + 1;
      }
      return true;
    }

    // Values held by all docs in [firstDoc, endDoc), including sparse columns.
    // A boundary is the start rank of the next doc with a value.
    std::pair<int64_t, int64_t> range(int32_t firstDoc, int32_t endDoc) {
      if (firstDoc >= endDoc) return {0, 0};
      if (shape->docs.allRows() && !shape->endValueRanks) {
        return {std::min(firstDoc, shape->docs.docsWithField()), std::min(endDoc, shape->docs.docsWithField())};
      }
      auto boundary = [&](int32_t doc) -> int64_t {
        if (seek(doc)) return begin();
        int32_t rank = shape->docs.allRows() || found == PostingsReader::END
            ? shape->docs.docsWithField() : docsIter.rank();
        if (!shape->endValueRanks) return rank;
        return rank == 0 ? 0 : shape->endValueRanks.valueAt(rank - 1);
      };
      int64_t startRank = boundary(firstDoc);
      return {startRank, boundary(endDoc)};
    }

    int32_t rank() const noexcept { return docRank; }
    int64_t begin() const noexcept { return first; }
    int64_t end() const noexcept { return last; }
    int64_t count() const noexcept { return last - first; }
  };
};

} // namespace luxir
