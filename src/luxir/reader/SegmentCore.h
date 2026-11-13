// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PostingsReader.h"
#include "VersionRuns.h"
#include "luxir/store/Directory.h"

namespace luxir {

// Immutable segment bundle. Shared across snapshots and with the writer;
// liveness and overlays belong to the snapshot, not these base files.
class SegmentCore {
  // Indexed by physical filenum and intentionally sparse. Segment finalize
  // folds buffered streams without renaming survivors, which keeps already
  // spilled filesystem/object-store keys stable.
  std::vector<std::shared_ptr<InputFile>> files;
  std::vector<InputStream> inputStreams;
  int64_t segInfoOffset;  // after this is segInfo, before this is the field index
  VersionRuns versions;

  const uint64_t segId;
  const uint64_t creatorEpoch;
  PostingsReader root;

  SegmentCore(uint64_t segId, uint64_t creatorEpoch)
      : segId(segId), creatorEpoch(creatorEpoch), root(*this) {}
  bool initializeFromFiles(Directory& dir, bool missingFileOK, bool expectSynced);

public:
  static std::shared_ptr<SegmentCore> create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                            bool missingFileOK = false, bool expectSynced = false);
  explicit SegmentCore(Directory& dir, uint64_t segId, uint64_t creatorEpoch);
  SegmentCore(const SegmentCore&) = delete;
  SegmentCore& operator=(const SegmentCore&) = delete;

  uint64_t id() const noexcept { return segId; }
  uint64_t epoch() const noexcept { return creatorEpoch; }
  PostingsReader& rootReader() noexcept { return root; }
  const PostingsReader& rootReader() const noexcept { return root; }
  const VersionRuns& versionRuns() const noexcept { return versions; }

  // Total bytes in the physical base-segment files this core owns.
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

  // We can't get & cache the InputStream here unless we create them all in the constructor (for thread safety)
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

  void checkFieldLocations(const SegFieldInfo& field) const;
};

} // namespace luxir
