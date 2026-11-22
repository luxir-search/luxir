// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>

#include "luxir/reader/FieldRecordCodec.h"
#include "luxir/reader/Space.h"

namespace luxir {

// Dictionary and postings metadata from a completed field.
struct WrittenTerms {
  field_parts::IndexedTotals totals;
  field_parts::Terms terms;
  uint8_t segFlags;  // See FieldSegmentFlags.
  field_parts::Bounds bounds = {};
};

// Presence recorded by a finished bitmap writer.
struct WrittenPresence {
  int32_t docsWithField = 0;
  field_parts::Slice bitmap;  // Null when a bitmap was not written.
};

// A finished value column and the row and rank data needed to address it.
template <class T>
struct WrittenColumn {
  T column;  // The exact descriptor written by the column codec.
  int64_t numValues = 0;
  WrittenPresence presence;
  field_parts::Slice endValueRanks;  // End value rank per doc with the field.
  field_parts::Slice owners;  // Owner doc per vector value.
  field_parts::Bounds bounds;  // Bounds supplied by the column writer, if any.
};

// A numeric payload before the caller supplies its row and rank data.
struct WrittenNumberColumn {
  field_parts::NumberColumn column;
  int64_t numValues;
  field_parts::ScalarBounds bounds;  // Empty when no values were written.
};

// A field being assembled by a flush or merge before writing its catalog record.
struct IndexFieldInfo {
  FieldRecordHeader header;  // Set type and flags before installing parts.
  SpaceOrdinal space = SpaceOrdinal::ROOT;  // Row space that owns this field.
  field_parts::Terms terms;  // Dictionary and its runs, with runs in the writer pool.
  // COLUMN holds only the descriptor selected by the field type and flags.
  union {
    field_parts::NumberColumn number = {};
    field_parts::OrdColumn ord;
    field_parts::BytesColumn bytes;
  };
  field_parts::Points points;
  field_parts::Norms norms;
  field_parts::Stored stored;  // Descriptor for a stored-fields resource.
  field_parts::Slice docs;  // Docs-with-field bitmap.
  field_parts::Slice endValueRanks;  // Monotonic end rank per doc with the field.
  field_parts::Slice owners;  // Monotonic owner doc per vector value.

  void setBounds(field_parts::Bounds value) {
    if (std::holds_alternative<std::monostate>(value)) return;
    assert(std::holds_alternative<std::monostate>(header.bounds));
    header.bounds = std::move(value);
  }

  void setPostings(WrittenTerms written) {
    assert(terms.location.isNull());
    assert(!written.terms.location.isNull() == (written.totals.numTerms != 0));
    header.segFlags = written.segFlags;
    // Only indexed fields describe dictionary and posting streams in the catalog.
    if (!(header.flags & FieldType::INDEX_DOCS)) return;
    header.indexedTotals = written.totals;
    terms = written.terms;
    header.parts.set(FieldPart::TERMS, !terms.location.isNull());
    setBounds(std::move(written.bounds));
  }

  void setPresence(WrittenPresence presence) {
    header.docsWithField = presence.docsWithField;
    docs = presence.bitmap;
    header.parts.set(FieldPart::DOCS, bool(docs));
  }

  void setColumn(WrittenColumn<field_parts::NumberColumn> written) {
    installColumn(number, written, field_parts::ColumnKind::NUMBER);
  }

  void setColumn(WrittenColumn<field_parts::OrdColumn> written) {
    installColumn(ord, written, field_parts::ColumnKind::ORD);
  }

  void setColumn(WrittenColumn<field_parts::BytesColumn> written) {
    installColumn(bytes, written, field_parts::ColumnKind::BYTES);
  }

  void setPoints(field_parts::Points value) {
    points = value;
    header.parts.set(FieldPart::POINTS, !points.location.isNull());
  }

  void setNorms(field_parts::Norms value) {
    norms = value;
    header.parts.set(FieldPart::NORMS, !norms.location.isNull());
  }

  void setStored(field_parts::Stored value) {
    stored = value;
    header.parts.set(FieldPart::STORED, !stored.location.isNull());
  }

private:
  template <class T>
  void installColumn(T& column, const WrittenColumn<T>& written, field_parts::ColumnKind kind) {
    if (!written.presence.docsWithField) return;
    if (field_parts::columnKind(header.type, header.flags) != kind) {
      throw std::logic_error("column kind does not match field type and flags");
    }
    std::construct_at(&column, written.column);
    header.parts.add(FieldPart::COLUMN);
    header.numValues = written.numValues;
    setPresence(written.presence);
    endValueRanks = written.endValueRanks;
    owners = written.owners;
    header.parts.set(FieldPart::VALUE_RANKS, bool(endValueRanks));
    header.parts.set(FieldPart::OWNERS, bool(owners));
    setBounds(written.bounds);
  }
};

} // namespace luxir
