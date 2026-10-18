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
#include "luxir/store/Directory.h"
#include "luxir/store/InputStream.h"

namespace luxir {

class PostingsReader;
class FieldReader;
class TermsEnum;
class DocsEnum;


// Lowest level postings reader class that needs to correspond to the PostingsWriter class that created the data.
// PostingsReader should be thread-safe at the top level, but any iterators it supplies would not be.
// This does not contain deleted docs, so one instance serves every index snapshot that holds the segment
// (IndexReader::openSnapshot shares it from the writer or the previous snapshot, and the writer from
// the published snapshot), and with it the views derived from the segment's bytes that it caches.
class PostingsReader {
  // Indexed by physical filenum and intentionally sparse. Segment finalize
  // folds buffered streams without renaming survivors, which keeps already
  // spilled filesystem/object-store keys stable.
  std::vector<std::shared_ptr<InputFile>> files;
  std::vector<InputStream> inputStreams;
  int64_t segInfoOffset;  // after this is segInfo, before this is the field index
  int32_t maxdoc;

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

  // Static factory method to create PostingsReader with optional handling of missing files.
  // Returns nullptr if missingFileOK=true and any required files are missing.
  // If expectSynced=true, the opened files are expected to have been fsynced (committed state).
  static std::shared_ptr<PostingsReader> create(Directory& dir, uint64_t segId, bool missingFileOK = false, bool expectSynced = false);

  explicit PostingsReader(Directory& dir, uint64_t segId);
  ~PostingsReader();
  PostingsReader(const PostingsReader&) = delete;
  PostingsReader& operator=(const PostingsReader&) = delete;

private:
  // Private default constructor for factory method
  PostingsReader() = default;

  // Initialize from files, returns false if missing files and missingFileOK=true
  bool initializeFromFiles(Directory& dir, uint64_t segId, bool missingFileOK, bool expectSynced = false);

public:

  int32_t maxDoc() const noexcept {
    return maxdoc;
  }

  int32_t numFields() const noexcept { return nFields; }

  // Immutable headers, decoded on first use and shared by all snapshots.
  // Missing names and ordinals outside the field index return nullptr.
  int32_t findOrdinal(std::string_view field) const;
  const Field* fieldInfo(std::string_view field);
  const Field* fieldInfo(int32_t ordinal);

  struct FieldCacheStats {
    size_t pages = 0;
    size_t entries = 0;
    size_t bytes = 0;
  };
  FieldCacheStats fieldCacheStats() const noexcept;

  // Total bytes in the physical base-segment files this reader can merge.
  uint64_t sizeInBytes() const noexcept {
    uint64_t total = 0;
    for (const auto& file : files) {
      if (file == nullptr) continue;
      uint64_t bytes = (uint64_t) file->size();
      total = bytes > std::numeric_limits<uint64_t>::max() - total
          ? std::numeric_limits<uint64_t>::max() : total + bytes;
    }
    return total;
  }

  InputFile* getFile(uint32_t fnum) {
    assert(fnum < files.size());
    assert(files[fnum] != nullptr);
    return files[fnum].get();
  }

  // We can't get & cache the InputStream in PostingsReader unless we create them all in the constructor (for thread safety)
  // But if we're using mmap, that's probably fine?  Would not be fine if we need to read everything in the constructor.
  // TODO: could also have a mode that opens on demand (and hence synchronizes)... that would be good for something like IndexWriter
  // that needs to only read the ID field to handle overwrites / deletions.  That file *might* already be open by another IndexReader
  // though?  How to coordinate?
  InputStream getInputStream(uint32_t fnum) {
    assert(fnum < inputStreams.size());
    assert(files[fnum] != nullptr);
    return inputStreams[fnum];
  }

  // return an InputStream positioned on the current location specified
  InputStream getInputStreamSeek(seg_location sloc) {
    InputStream is = getInputStream(sloc.filenum());
    is.seek(sloc.offset());
    return is;
  }

  friend class FieldReader;
};



} // end namespace
