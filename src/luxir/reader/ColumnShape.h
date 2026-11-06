// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>

#include "DocsReader.h"
#include "IntColReader.h"
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

    int32_t rank() const noexcept { return docRank; }
    int64_t begin() const noexcept { return first; }
    int64_t end() const noexcept { return last; }
    int64_t count() const noexcept { return last - first; }
  };
};

} // namespace luxir
