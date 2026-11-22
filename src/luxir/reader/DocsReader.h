// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
#include "PostingsReader.h"
#include "ReaderErrors.h"
#include "luxir/util/screaming.h"

namespace luxir {
// Maps doc IDs to ranks among the docs that have a field.
class DocsReader {
  screaming::BitSet bits;  // The docs bitmap for a sparse field.
  int32_t count;  // Number of docs that have the field.
  bool every;  // Every doc has the field, so its rank is its ID.

public:
  DocsReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : count(fieldInfo.docsWithField), every(count == reader.maxDoc()) {
    if (every) return;
    auto bitmap = fieldInfo.docsBitmap();
    assert(bitmap);
    if (!bits.set(reader.core().slice(bitmap)) || bits.empty()) {
      segmentReadContext(reader.core(), fieldInfo, bitmap.location.filenum()).invalid("invalid docs bitmap");
    }
  }

  // Standalone columns have one value per doc and no docs bitmap.
  explicit DocsReader(int32_t docsWithField) : count(docsWithField), every(docsWithField != 0) {}

  int32_t docsWithField() const noexcept { return count; }
  bool noRows() const noexcept { return count == 0; }
  bool allRows() const noexcept { return every; }

  const screaming::BitSet* bitset() const noexcept {
    return bits.empty() ? nullptr : &bits;
  }

  // Walks the compressed docs bitmap. The iterator is empty for either
  // count-only state. Dense column walks handle allRows() directly.
  screaming::BitSet::Iterator bitmapIterator() const noexcept {
    return screaming::BitSet::Iterator(bits);
  }

  // Rank among docs with this field, or -1 when the doc is missing.
  // Per-doc callers test allRows() first: then the rank is the doc id and
  // this is never reached on their dense path.
  int32_t rankOf(int32_t doc) const {
    assert(doc >= 0);
    if (every) return doc;
    if (bits.empty()) return -1;
    auto iter = bitmapIterator();
    return iter.advance(doc) == doc ? iter.rank() : -1;
  }
};

} // namespace luxir
