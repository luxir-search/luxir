// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <format>
#include <memory>
#include <optional>

#include "FieldParts.h"
#include "FieldStats.h"
#include "ReaderErrors.h"

namespace luxir {

// Each field record starts with its packed name, type, schema flags and
// type parameters when its type has them. The part mask and state byte follow.
// The state holds segment flags and bits for bounds and an explicit value count.
// The counts are docs with the field, the optional value count, then indexed
// totals for indexed fields. The type's bounds follow when present.
// Present parts follow in FieldPart order without frames.
//   Terms have a singleton stream mask or run count tag and packed stream masks,
//   dictionary length, trie length and root, non-final run counts and lengths,
//   dictionary location, then each run's present posting slices.
//   Number columns have a location and the blocks and index lengths. Ord columns
//   have options, a location and the predicted payload length when needed.
//   Bytes columns have options, a values slice and optional end offsets.
//   Points have a location, leaves length and leaf capacity.
//   Norms have options and a location.
//   Stored resources have chunk counts and maximum size, codec and chunk limits,
//   location and three lengths, then first-doc and chunk-offset slices.
//   The docs bitmap, end value ranks and owners are standalone slices.
// Counts and locations are unsigned varints. Slice locations give a file number
// and absolute offset, followed by a byte length for a standalone slice.

// The kinds of data a field can have. A record lists which are present.
enum class FieldPart : uint8_t { TERMS, COLUMN, POINTS, NORMS, STORED, DOCS, VALUE_RANKS, OWNERS, COUNT };

// Tracks which parts a record has.
class FieldPartSet {
  friend class DecodedFieldRecord;
  uint16_t members = 0;  // One bit per FieldPart.

  // Finds the part's slot by counting the present parts before it.
  constexpr int rank(FieldPart part) const noexcept {
    return std::popcount((unsigned)(members & ((1u << (unsigned)part) - 1)));
  }

public:
  constexpr FieldPartSet() noexcept = default;
  explicit constexpr FieldPartSet(uint16_t mask) noexcept : members(mask) {}

  constexpr bool has(FieldPart part) const noexcept { return (members & (1u << (unsigned)part)) != 0; }
  constexpr void set(FieldPart part, bool present) noexcept {
    if (present) add(part);
    else members &= ~(1u << (unsigned)part);
  }
  constexpr void add(FieldPart part) noexcept { members |= 1u << (unsigned)part; }
  constexpr uint16_t mask() const noexcept { return members; }
  constexpr int size() const noexcept { return std::popcount(members); }
  bool operator==(const FieldPartSet&) const = default;
};

// Holds facts about the segment data. Schema flags hold the declaration.
namespace FieldSegmentFlags {
inline constexpr uint8_t MAY_HAVE_FITTED = 1u << 0;  // A term may stand in for a long value.
inline constexpr uint8_t ALL = MAY_HAVE_FITTED;
}

// The portion of a field record readable without decoding any part.
struct FieldRecordHeader {
  PackedTerm name = PackedTerm(nullptr);  // Physical name in the writer's pool or mapped record.
  FieldType::Type type = FieldType::NONE;  // Physical field type.
  FieldType::flag_type flags = 0;  // Schema flags as of indexing, excluding ABSTRACT and DERIVED.
  uint32_t typeParameters = 0;  // Type-specific settings that affect the data.
  uint8_t segFlags = 0;  // See FieldSegmentFlags.
  FieldPartSet parts;  // Which parts the record contains.
  int32_t docsWithField = 0;  // Docs in the row space that have the field.
  int64_t numValues = 0;  // Values in the column, zero without a column.
  field_parts::IndexedTotals indexedTotals;  // Zero for an unindexed field.
  field_parts::Bounds bounds;  // Value range for pruning, if available.

  bool operator==(const FieldRecordHeader&) const = default;
};

// Segments sharing a schema field must agree on the persisted type and its parameters.
inline void requireCompatibleType(const auto& field, const auto& other) {
  if (field.type != other.type || field.typeParameters != other.typeParameters) {
    throw std::runtime_error(std::format(
        "Incompatible persisted types for field '{}': {} and {}",
        std::string_view(field.name), FieldType::describeType(field.type, field.typeParameters),
        FieldType::describeType(other.type, other.typeParameters)));
  }
}

// Byte offset from the start of the decoded record to one part.
using PartOffset = uint16_t;

class DecodedFieldRecord;
using FieldRecordPtr = std::unique_ptr<const DecodedFieldRecord>;

struct IndexFieldInfo;

namespace FieldRecordCodec {
// The record's counts are checked against the row space's maxDoc.
void encode(OutputStream& out, const IndexFieldInfo& fieldInfo, int32_t maxDoc);
// Reads the header without decoding any part.
FieldRecordHeader fieldHeader(std::span<const char> bytes, int32_t maxDoc, size_t* consumed = nullptr,
                             SegmentReadContext context = SegmentReadContext());
// The file inventory bounds each slice. The ordinal is the record's field table position.
FieldRecordPtr decode(std::span<const char> bytes, int32_t maxDoc, field_parts::FileSizes fileSizes,
                      int32_t ordinal, SegmentReadContext context = SegmentReadContext());
} // namespace FieldRecordCodec

// A field's record as readers use it ("fieldInfo"). One heap block of exactly the size
// the field needs: this fixed portion, then only the totals, bounds, parts and term
// runs the field actually has, reached through the offsets below. It is immutable
// and borrows the mapped catalog bytes for the name and dictionary bounds. The segment must
// outlive it.
class DecodedFieldRecord {
public:
  const PackedTerm name;  // Physical name in the mapped record.
  const int64_t numValues;  // Values in the column, zero without a column.
  const FieldType::Type type;  // Physical field type.
  const FieldType::flag_type flags;  // Schema flags as of indexing, excluding ABSTRACT and DERIVED.
  const uint32_t typeParameters;  // Type-specific settings that affect the data.
  const int32_t docsWithField;  // Docs in the row space that have the field.
  const int32_t ordinal;  // Position in the space's field table and key in the reader's per-field caches.

private:
  friend FieldRecordPtr FieldRecordCodec::decode(std::span<const char>, int32_t, field_parts::FileSizes,
                                                int32_t, SegmentReadContext);
  const FieldPartSet parts;  // Which parts have slots in the table.

public:
  const uint8_t segFlags;  // See FieldSegmentFlags.

private:
  const bool hasBounds;  // Whether the type's bounds follow the totals.

  DecodedFieldRecord(const FieldRecordHeader& header, int32_t ordinal);
  DecodedFieldRecord(const DecodedFieldRecord&) = delete;

  // The table follows the fixed record. Totals and bounds require 8-byte alignment.
  static uint32_t totalsOffset(FieldPartSet parts) noexcept {
    return (uint32_t)alignUp(sizeof(DecodedFieldRecord) + parts.size() * sizeof(PartOffset), alignof(uint64_t));
  }

  static uint32_t boundsOffset(FieldPartSet parts, FieldType::flag_type flags) noexcept {
    return totalsOffset(parts) + ((flags & FieldType::INDEX_DOCS) ? sizeof(field_parts::IndexedTotals) : 0);
  }

  uint32_t boundsOffset() const noexcept { return boundsOffset(parts, flags); }

  template <class T>
  const T& at(uint32_t offset) const noexcept {
    return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(this) + offset);
  }

  template <class T>
  const T* partAt(FieldPart part) const noexcept {
    if (!parts.has(part)) return nullptr;
    return &at<T>(at<PartOffset>(sizeof(DecodedFieldRecord) + parts.rank(part) * sizeof(PartOffset)));
  }

  field_parts::Slice partSlice(FieldPart part) const noexcept {
    const auto* slice = partAt<field_parts::Slice>(part);
    return slice ? *slice : field_parts::Slice{};
  }

  // The column part holds exactly the kind the field's type and flags call for.
  template <class T>
  const T* columnAt(field_parts::ColumnKind kind) const noexcept {
    return field_parts::columnKind(type, flags) == kind ? partAt<T>(FieldPart::COLUMN) : nullptr;
  }

  // The bounds slot holds exactly the kind the field's type and flags call for.
  template <class T>
  const T* boundsAt(field_parts::BoundsKind kind) const noexcept {
    return hasBounds && field_parts::boundsKind(type, flags) == kind ? &at<T>(boundsOffset()) : nullptr;
  }

public:
  // The block includes its parts and runs after the fixed record.
  static void operator delete(void* block) noexcept { ::operator delete(block); }

  // Dictionary and postings statistics. Unindexed fields share a zero value.
  const field_parts::IndexedTotals& indexedTotals() const noexcept {
    static constexpr field_parts::IndexedTotals empty;
    return (flags & FieldType::INDEX_DOCS) ? at<field_parts::IndexedTotals>(totalsOffset(parts)) : empty;
  }

  // Every doc with the field has exactly one term, so no doc is in two postings lists.
  bool hasDisjointTermPostings() const noexcept {
    return (flags & FieldType::INDEX_DOCS) && (type == FieldType::STRING || type == FieldType::ID)
        && indexedTotals().sumDocFreq == docsWithField;
  }

  uint32_t allocatedBytes() const noexcept;
  FieldPartSet partSet() const noexcept { return parts; }

  // Each part, or null when the field does not have it.
  const field_parts::Terms* terms() const noexcept { return partAt<field_parts::Terms>(FieldPart::TERMS); }
  const field_parts::Points* points() const noexcept { return partAt<field_parts::Points>(FieldPart::POINTS); }
  const field_parts::Norms* norms() const noexcept { return partAt<field_parts::Norms>(FieldPart::NORMS); }
  const field_parts::Stored* stored() const noexcept { return partAt<field_parts::Stored>(FieldPart::STORED); }


  // Derives layout from the record. Returns a null slice
  // when the part is absent or the layout has no such slice.
  int32_t termBlockCount() const noexcept;
  field_parts::Slice termBlockOffsets() const;
  field_parts::Slice termTrie() const;
  uint64_t pointCount() const noexcept;
  uint64_t pointLeafCount() const noexcept;
  field_parts::Slice pointsDirectory() const;
  field_parts::Slice normsValues(int32_t maxDoc) const;
  field_parts::Slice ordBlockMetadata(int32_t maxDoc) const;
  int32_t bytesStride() const noexcept;

  bool hasColumn() const noexcept { return parts.has(FieldPart::COLUMN); }

  // The column, if the field has one of this kind. A field has at most one column.
  const field_parts::NumberColumn* numberColumn() const noexcept {
    return columnAt<field_parts::NumberColumn>(field_parts::ColumnKind::NUMBER);
  }

  const field_parts::OrdColumn* ordColumn() const noexcept {
    return columnAt<field_parts::OrdColumn>(field_parts::ColumnKind::ORD);
  }

  const field_parts::BytesColumn* bytesColumn() const noexcept {
    return columnAt<field_parts::BytesColumn>(field_parts::ColumnKind::BYTES);
  }

  // Returns the docs bitmap for a sparse field.
  field_parts::Slice docsBitmap() const noexcept { return partSlice(FieldPart::DOCS); }

  // Monotonic end value ranks by docs-with-field rank.
  field_parts::Slice endValueRanks() const noexcept { return partSlice(FieldPart::VALUE_RANKS); }

  // Monotonic owner doc IDs by vector value rank.
  field_parts::Slice owners() const noexcept { return partSlice(FieldPart::OWNERS); }

  // Each accessor returns null unless the field has bounds of that kind.
  const field_parts::ScalarBounds* scalarBounds() const noexcept {
    return boundsAt<field_parts::ScalarBounds>(field_parts::BoundsKind::SCALAR);
  }

  const field_parts::DictionaryBounds* dictionaryBounds() const noexcept {
    return boundsAt<field_parts::DictionaryBounds>(field_parts::BoundsKind::DICTIONARY);
  }

  const field_parts::GeoBounds* geoBounds() const noexcept {
    return boundsAt<field_parts::GeoBounds>(field_parts::BoundsKind::GEO);
  }

  const field_parts::IntervalBounds* intervalBounds() const noexcept {
    return boundsAt<field_parts::IntervalBounds>(field_parts::BoundsKind::INTERVAL);
  }
};

} // namespace luxir
