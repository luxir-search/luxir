// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "luxir/reader/FieldRecordCodec.h"
#include "luxir/search/IndexReader.h"

namespace luxir {

// Read-only evidence for Query::mayMatch: the field record headers of one
// segment of a reader, or of every segment at once (the collection view,
// whose headers are the segments' headers merged). Headers come from the
// segments' catalogs through IndexReader::field(), memoized per field and
// reader; no part is decoded and no data is read.
//
// Headers describe the segments as written, deleted documents included, so
// they prove "no document matches", never a count. A field without a record
// is absent from the view's row space.
class CatalogSummaryView {
  IndexReader& reader;
  int32_t ord;           // Segment ordinal, or COLLECTION.
  std::string* reason;   // Receives the first proof's reason, when given.
  bool* opaqueSeen;      // Set when a query without proofs was evaluated.

public:
  static constexpr int32_t COLLECTION = -1;

  CatalogSummaryView(IndexReader& reader, int32_t ord, std::string* reason = nullptr,
                     bool* opaqueSeen = nullptr)
      : reader(reader), ord(ord), reason(reason), opaqueSeen(opaqueSeen) {}

  IndexReader& indexReader() const noexcept { return reader; }
  int32_t segment() const noexcept { return ord; }
  bool collection() const noexcept { return ord == COLLECTION; }

  // A collection proof answers before any weight exists, so it must look up
  // every field the query uses (IndexReader::field() raises a type mismatch
  // as weight creation would): composites evaluate every child, prohibited
  // ones included, instead of stopping at the first proof.
  bool exhaustive() const noexcept { return collection(); }

  // The physical field's header, or null when the view's row space has no
  // record of it (in this segment; in any segment for the collection view).
  const FieldRecordHeader* header(std::string_view physicalField) const {
    auto fields = reader.field(physicalField);
    if (!fields.exists()) return nullptr;
    return collection() ? &fields.mergedHeader() : fields.header((size_t)ord);
  }

  // The same segment (or the collection) in another row space of the
  // snapshot, such as a nested path. Null when that space has no rows here.
  std::optional<CatalogSummaryView> inSpace(IndexReader& space) const {
    int64_t rows = collection() ? space.maxDoc() : (int64_t)space.segments()[(size_t)ord].maxDoc();
    if (rows == 0) return std::nullopt;
    return CatalogSummaryView(space, ord, reason, opaqueSeen);
  }

  // Records why a leaf proved the view empty; the first reason wins.
  void prove(std::string_view why) const {
    if (reason != nullptr && reason->empty()) reason->assign(why);
  }

  // Called by queries that offer no proof and may read fields the view
  // never looked up; a collection proof then stands aside.
  void opaque() const noexcept {
    if (opaqueSeen != nullptr) *opaqueSeen = true;
  }
};

} // namespace luxir
