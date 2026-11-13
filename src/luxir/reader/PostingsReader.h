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
#include "SegFieldInfo.h"
#include "StoredFieldsMetadata.h"
#include "luxir/store/InputStream.h"

namespace luxir {

class SegmentCore;
class FieldReader;
class TermsEnum;
class DocsEnum;


// Concrete reader of one dense row space within a segment core. Its immutable
// field catalog and lazy decoded headers live with the core; iterators are local
// to callers. Liveness belongs to the snapshot's leaf.
class PostingsReader {
  SegmentCore& segmentCore;
  int32_t maxdoc = 0;

  // Ordinals follow the sorted field index. Only the directory is allocated
  // at open; slot pages and immutable headers are published on first use.
  struct FieldPage;
  static constexpr size_t FIELDS_PER_PAGE = 64;
  int32_t nFields = 0;
  const uint32_t* fieldOffsets = nullptr;
  int64_t fieldOffsetsLoc = 0;
  std::unique_ptr<std::atomic<FieldPage*>[]> fieldPages;

public:
  struct Field : SegFieldInfo {
    struct ColumnBounds {
      int64_t min, max;
    };
    std::optional<ColumnBounds> columnBounds;
    // Field-sort planning reads every segment's leaf bounds. Keep the view
    // with the header rather than rebuilding it for each request.
    std::optional<NumColumnFormat::LeafMeta> leafMeta;
    std::unique_ptr<const StoredFieldsMetadata> stored;

    size_t allocatedBytes() const {
      return sizeof(*this) + (stored ? stored->allocatedBytes() : 0);
    }
    const NumColumnFormat::LeafMeta* numericLeafMeta() const noexcept {
      return leafMeta ? &*leafMeta : nullptr;
    }
  };

  // used as a sentinel value for docs and positions iterators in a single segment.
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
  static constexpr int32_t HARD_MAX_DOC = END - (1 << 20);

  ~PostingsReader();
  PostingsReader(const PostingsReader&) = delete;
  PostingsReader& operator=(const PostingsReader&) = delete;

private:
  friend class SegmentCore;
  explicit PostingsReader(SegmentCore& core) : segmentCore(core) {}
  void initialize(int32_t maxDoc, int64_t fieldOffsetsLoc, const uint32_t* fieldOffsets, int32_t count);

public:

  int32_t maxDoc() const noexcept {
    return maxdoc;
  }

  int32_t numFields() const noexcept { return nFields; }
  SegmentCore& core() const noexcept { return segmentCore; }

  // Immutable headers, decoded on first use and shared by all snapshots.
  // Missing names and ordinals outside the field index return nullptr.
  int32_t findOrdinal(std::string_view field) const;
  const Field* fieldInfo(std::string_view field);
  const Field* fieldInfo(int32_t ordinal);

  struct FieldCacheStats {
    size_t pages = 0;
    size_t entries = 0;
    size_t entryBytes = 0;
    size_t bytes = 0;
  };
  FieldCacheStats fieldCacheStats() const noexcept;

  friend class FieldReader;
};



} // end namespace
