// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>

#include "luxir/schema/FieldType.h"
#include "luxir/store/OutputStream.h"
#include "Postings.h"

// A field record locates the data for one field and carries the layout numbers
// needed to open it. Writers supply these descriptors and readers use them to
// open the data.
namespace luxir::field_parts {

// Zero marks an absent file. File 0's data ends at the catalog.
using FileSizes = std::span<const uint64_t>;

// A byte range in one of the segment's files: everything needed to map or
// range-read one piece of data.
struct Slice {
  seg_location location;  // File number and offset of the first byte.
  uint64_t bytes = 0;     // Length of the slice.

  // A present slice may have zero bytes.
  bool has_value() const noexcept { return !location.isNull(); }
  explicit operator bool() const noexcept { return has_value(); }

  seg_location end() const { return location.addOffset(bytes); }

  Slice subSlice(uint64_t offset, uint64_t length) const {
    if (offset > bytes || length > bytes - offset) {
      throw std::out_of_range("slice outside stream");
    }
    return {location.addOffset(offset), length};
  }

  bool operator==(const Slice&) const = default;
};

// Byte length of the monotonic payload before its block metadata, if the slice fits.
std::optional<uint64_t> monotonicBlocksBytes(Slice slice, int64_t numValues) noexcept;

// Consecutive term blocks with a common base for dictionary and posting offsets.
struct TermRun {
  int32_t firstTermOrd;      // Ordinal within the field of this run's first term.
  int32_t firstBlockOrd;     // Ordinal within the field of this run's first dictionary block.
  uint64_t dictionaryStart;  // Byte offset of this run's first block within the field's dictionary.
  Slice docs;                // Doc IDs and frequencies, null for an empty stream.
  Slice positions;           // Positions, null for an empty stream.

  int32_t blockForTerm(int64_t termOrd) const noexcept {
    return (int32_t)(firstBlockOrd + (termOrd - firstTermOrd) / Postings::TERMS_BLOCK_SIZE);
  }

  int64_t firstTermForBlock(int32_t blockOrd) const noexcept {
    return firstTermOrd + (int64_t)(blockOrd - firstBlockOrd) * Postings::TERMS_BLOCK_SIZE;
  }

  bool operator==(const TermRun&) const = default;
};

// Dictionary blocks, the aligned offset table, then the trie on one checkout.
struct Terms {
  using BlockOffset = uint32_t;
  static constexpr size_t OFFSETS_ALIGN = alignof(BlockOffset);

  seg_location location;          // First dictionary block.
  uint64_t dictionaryBytes = 0;   // Dictionary blocks up to the alignment padding.
  uint64_t trieBytes = 0;         // Trie extent including decoder slack.
  uint64_t trieRootOffset = 0;    // Root offset relative to the start of the trie.
  std::span<const TermRun> runs;  // Run starts in term order.

  Slice dictionaryBlocks() const { return {location, dictionaryBytes}; }

  bool operator==(const Terms& other) const {
    return location == other.location && dictionaryBytes == other.dictionaryBytes && trieBytes == other.trieBytes
        && trieRootOffset == other.trieRootOffset && std::ranges::equal(runs, other.runs);
  }
};

// Dictionary layout derived from the last run and the indexed term count.
int32_t termBlockCount(const Terms& terms, int64_t numTerms) noexcept;
uint64_t termBlockOffsetsStart(uint64_t dictionaryEnd) noexcept;
Slice termBlockOffsets(const Terms& terms, int64_t numTerms);
Slice termTrie(Slice offsets, uint64_t trieBytes);

// Column of int64 values (integers, sortable float bits, dates, geo and interval
// encodings), opened by IntColReader.
struct NumberColumn {
  seg_location location;     // Value blocks, alignment padding, then the index.
  uint64_t blocksBytes = 0;  // Encoded payload, excluding padding.
  uint64_t indexBytes = 0;   // Block metadata, zones and leaf statistics.

  Slice blocks() const { return {location, blocksBytes}; }
  Slice index() const;
  bool operator==(const NumberColumn&) const = default;
};

// DIRECT packs fixed-width ordinals. PREDICTED packs residuals against a
// line fitted to each block of 4096 values. The fit is in the block metadata.
enum class OrdMode : uint8_t { DIRECT, PREDICTED, LAST = PREDICTED };
// DOC_ID stores one entry per doc ID and uses zero for missing values.
// RANK stores one entry per value, addressed through the doc rank.
enum class OrdIndexing : uint8_t { DOC_ID, RANK, LAST = RANK };

// Column of term ordinals for a field that also has a term dictionary, opened by
// OrdColReader.
struct OrdColumn {
  seg_location location;                       // Packed values followed immediately by predicted metadata.
  uint64_t valuesBytes = 0;                    // Packed payload length, derived for DIRECT.
  OrdMode mode = OrdMode::DIRECT;              // Packing mode.
  OrdIndexing indexing = OrdIndexing::DOC_ID;  // Whether entries follow doc IDs or value ranks.

  // Ordinals may be addressed by doc ID or by value rank.
  static uint64_t encodedCount(OrdIndexing indexing, int32_t maxDoc, int64_t numValues) noexcept {
    return indexing == OrdIndexing::DOC_ID ? maxDoc : numValues;
  }
  Slice values() const { return {location, valuesBytes}; }
  bool operator==(const OrdColumn&) const = default;
};

// Null for a direct ord column, which has no metadata.
Slice ordBlockMetadata(const OrdColumn& column, uint64_t count);

// Column of concatenated byte strings, opened by StrColReader and VectorReader.
struct BytesColumn {
  Slice values;      // All values in rank order.
  Slice endOffsets;  // Monotonic end byte offsets, null for fixed stride.
  uint8_t vectorEncoding = 0;  // VECTOR only: the values' VectorEncoding, as written for this segment.

  bool operator==(const BytesColumn&) const = default;
};

// Fixed stride in bytes, or -1 for variable offsets or an invalid fixed size.
int32_t bytesStride(const BytesColumn& column, int64_t numValues) noexcept;

// The kinds of column. A field has at most one column.
enum class ColumnKind : uint8_t { NONE, NUMBER, ORD, BYTES };

// The kind of column a field of this type and flags has, when it has one.
inline ColumnKind columnKind(FieldType::Type type, FieldType::flag_type flags) noexcept {
  switch (FieldType::storage(type)) {
    case FieldType::Storage::INT64: return ColumnKind::NUMBER;
    case FieldType::Storage::TERMS: return (flags & FieldType::INDEX_DOCS) ? ColumnKind::ORD : ColumnKind::BYTES;
    case FieldType::Storage::VECTOR: return ColumnKind::BYTES;
    default: return ColumnKind::NONE;
  }
}

// BKD tree over the field's values (numeric range, geo, interval), opened by
// PointsReader and BKDReader.
struct Points {
  seg_location location;          // Point leaves followed immediately by the directory.
  uint64_t leavesBytes = 0;       // Encoded (value, doc) leaf blocks.
  uint16_t maxPointsPerLeaf = 0;  // Leaf capacity, used to derive the directory length.

  Slice leaves() const { return {location, leavesBytes}; }
  bool operator==(const Points&) const = default;
};

// Each interval contributes one point for its two endpoint values.
uint64_t pointCount(FieldType::Type type, int64_t numValues) noexcept;
uint64_t pointLeafCount(const Points& points, FieldType::Type type, int64_t numValues) noexcept;
Slice pointsDirectory(const Points& points, FieldType::Type type, int64_t numValues);

// FLAT stores a byte per doc ID. SPARSE stores a byte per doc with the field in rank order.
enum class NormsMode : uint8_t { FLAT, SPARSE, LAST = SPARSE };

// Length norms of a text field, one byte per document.
struct Norms {
  seg_location location;             // One byte per doc selected by the mode.
  NormsMode mode = NormsMode::FLAT;  // Whether entries follow doc IDs or docs-with-field ranks.

  static uint64_t bytes(NormsMode mode, int32_t maxDoc, int32_t docsWithField) noexcept {
    return mode == NormsMode::FLAT ? maxDoc : docsWithField;
  }
  bool operator==(const Norms&) const = default;
};

// Extent of the norm bytes in the selected layout.
inline Slice normsValues(const Norms& norms, int32_t maxDoc, int32_t docsWithField) {
  return {norms.location, Norms::bytes(norms.mode, maxDoc, docsWithField)};
}

// A stored-fields resource: the retrievable values of every field assigned to it,
// compressed in chunks of consecutive documents. One record per resource per row
// space, not one per stored field. Opened by StoredFieldsReader.
struct Stored {
  int32_t numChunks = 0;         // Number of entries in firstDocs.
  int32_t numDirtyChunks = 0;    // Chunks sealed before reaching a chunk limit.
  int64_t maxChunkBytes = 0;     // Largest uncompressed chunk, for sizing scratch.
  // Compression codec for chunks.
  StoredChunkFormat::Codec codec = StoredChunkFormat::Codec::LZ4;
  uint32_t chunkTarget = 0;      // Uncompressed bytes at which a chunk is sealed.
  uint32_t maxDocsPerChunk = 0;  // Doc limit for one compressed chunk.
  seg_location location;         // Chunks, optional dictionary, then names.
  int64_t chunksBytes = 0;       // Compressed chunks, back to back.
  uint64_t dictionaryBytes = 0;  // Zero when no dictionary was trained.
  uint64_t namesBytes = 0;       // Member count followed by packed field names.
  Slice firstDocs;               // Monotonic first doc per chunk (numChunks values).
  Slice chunkOffsets;            // Monotonic chunk offsets ending at chunksBytes (numChunks + 1 values).

  // Persisted chunk layout. Compression level and training policy are schema settings.
  StoredChunkFormat format() const {
    return {.codec = codec, .chunkTarget = chunkTarget, .maxDocsPerChunk = maxDocsPerChunk};
  }
  Slice chunks() const { return {location, (uint64_t)chunksBytes}; }
  Slice dictionary() const {
    return dictionaryBytes ? Slice{location.addOffset(chunksBytes), dictionaryBytes} : Slice{};
  }
  Slice names() const { return {location.addOffset(chunksBytes).addOffset(dictionaryBytes), namesBytes}; }
  bool operator==(const Stored&) const = default;
};

} // namespace luxir::field_parts
