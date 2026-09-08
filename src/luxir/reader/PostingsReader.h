// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <limits>
#include <vector>
#include "luxir/store/Directory.h"
#include "luxir/store/InputStream.h"

namespace luxir {

class PostingsReader;
class FieldReader;
class TermsEnum;
class DocsEnum;


// Lowest level postings reader class that needs to correspond to the PostingsWriter class that created the data.
// PostingsReader should be thread-safe at the top level, but any iterators it supplies would not be.
// This does not contain deleted docs, so instances can be shared by different index versions.
class PostingsReader {
  // Indexed by physical filenum and intentionally sparse. Segment finalize
  // folds buffered streams without renaming survivors, which keeps already
  // spilled filesystem/object-store keys stable.
  std::vector<std::shared_ptr<InputFile>> files;
  std::vector<InputStream> inputStreams;
  int64_t segInfoOffset;  // after this is segInfo, before this is the field index
  int32_t maxdoc;
public:

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

private:
  // Private default constructor for factory method
  PostingsReader() = default;

  // Initialize from files, returns false if missing files and missingFileOK=true
  bool initializeFromFiles(Directory& dir, uint64_t segId, bool missingFileOK, bool expectSynced = false);

public:

  int32_t maxDoc() const noexcept {
    return maxdoc;
  }

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
