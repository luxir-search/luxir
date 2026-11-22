// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "luxir/codec/NumColumnFormat.h"
#include "FieldRecordCodec.h"
#include "Space.h"
#include "StoredFieldsMetadata.h"
#include "luxir/store/InputStream.h"

namespace luxir {

class SegmentCore;
class FieldCursor;
class TermsEnum;
class DocsEnum;


// Opens numeric leaf metadata and reports corrupt index bytes with their context.
NumColumnFormat::LeafMeta readNumericLeafMeta(std::span<const char> index, int64_t numValues,
                                              SegmentReadContext context = SegmentReadContext());

// Opens fields in one row space and shares the decoded records and metadata
// with all readers of the segment. Each snapshot supplies its own live docs.
class PostingsReader {
  SegmentCore& segmentCore;
  int32_t maxdoc = 0;  // Number of docs in the row space.
  const SpaceOrdinal space;

  // Ordinals follow the sorted field table. The array of page pointers is
  // allocated at open. Pages and field records are published on first use.
  struct FieldPage;
  using OpenedField = std::atomic<const NumColumnFormat::LeafMeta*>;
  struct StoredResource;
  std::atomic<StoredResource*> storedResources = nullptr;  // Published resource states, keyed by field ordinal.
  static constexpr size_t FIELDS_PER_PAGE = 64;
  int32_t nFields = 0;  // Number of records in its field table.
  std::unique_ptr<std::atomic<FieldPage*>[]> fieldPages;  // One slot per record page.

public:
  // Sentinel for docs and positions iterators in a segment.
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  // Soft cap on a segment's maxDoc: merge admission gates on it and an inverter
  // flushes at it (IndexWriter::perInverterMaxDocs), though the batch that trips
  // that check may carry a flushed segment a little past it.  Capping a full bit
  // below END makes doc-id arithmetic structurally safe: doc + c cannot overflow
  // int32 for any c <= 1 << 30, and can never collide with END, so window/block
  // cursor math needs no per-site overflow audits.
  static constexpr int32_t MAX_SEGMENT_DOCS = (1 << 30) - 1;

  // Where doc ids actually stop being safe, far above the soft cap: real cursor
  // offsets are window/block sized (DocsEnumMeta::L1_DOCS = 4096 is the largest),
  // so a doc id below this can neither overflow int32 nor reach END.  Asserts
  // check this rather than MAX_SEGMENT_DOCS, so they trip on a runaway doc id and
  // stay quiet on a legal overshoot of the soft cap.
  static constexpr int32_t HARD_MAX_DOC = HARD_MAX_ROW_COUNT;

  ~PostingsReader();
  PostingsReader(const PostingsReader&) = delete;
  PostingsReader& operator=(const PostingsReader&) = delete;

private:
  friend class SegmentCore;
  PostingsReader(SegmentCore& core, SpaceOrdinal space) : segmentCore(core), space(space) {}
  void initialize(int32_t maxDoc, int32_t count);
  PackedTerm fieldName(int32_t ordinal) const noexcept;
  static size_t numFieldPages(int32_t count) noexcept {
    return ((size_t)count + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE;
  }
  size_t fieldPageSize(size_t pageIndex) const noexcept;
  FieldPage& fieldPage(size_t pageIndex);
  OpenedField& openedField(const DecodedFieldRecord& fieldInfo);

public:

  int32_t maxDoc() const noexcept {
    return maxdoc;
  }

  SpaceOrdinal spaceOrdinal() const noexcept { return space; }
  int32_t numFields() const noexcept { return nFields; }
  SegmentCore& core() const noexcept { return segmentCore; }

  // Immutable field records, decoded on first use and shared by all snapshots.
  // Missing names and ordinals outside the field table return nullptr.
  int32_t findOrdinal(std::string_view field) const;
  const DecodedFieldRecord* fieldInfo(std::string_view field);
  const DecodedFieldRecord* fieldInfo(int32_t ordinal);
  FieldRecordHeader fieldHeader(int32_t ordinal) const;

  // Decode without publishing to the field cache.
  FieldRecordPtr decodeField(int32_t ordinal) const;


  // Acquired views are shared for the reader's lifetime.
  const NumColumnFormat::LeafMeta* openNumericLeafMeta(const DecodedFieldRecord& fieldInfo);
  const StoredFieldsMetadata* openStoredMetadata(const DecodedFieldRecord& fieldInfo);
  const StoredDecompressionDictionary* openStoredDictionary(const field_parts::Stored& stored,
                                                             const StoredFieldsMetadata& metadata);

  // Allocations owned by this row space's field cache.
  struct FieldCacheStats {
    size_t pages = 0;  // Published record pages.
    size_t pageBytes = 0;  // Record page allocations, excluding their decoded records.
    size_t entries = 0;  // Published field records.
    size_t entryBytes = 0;  // Whole allocations for published records.
    size_t bytes = 0;  // Total cache bytes including the page pointer array.
    size_t openedPages = 0;  // Published numeric metadata pages.
    size_t openedPageBytes = 0;  // Numeric metadata slot arrays.
    size_t openedEntries = 0;  // Published numeric views, resources and dictionaries.
    size_t openedEntryBytes = 0;  // Allocations owned by opened entries.
  };
  FieldCacheStats fieldCacheStats() const noexcept;

private:
  friend class FieldCursor;
};



} // end namespace
