// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PostingsReader.h"
#include "SegmentCatalog.h"
#include "CatalogRange.h"
#include "VersionRuns.h"
#include <span>
#include "luxir/store/Directory.h"

namespace luxir {
class OwnershipShape;
namespace api { struct SegmentInfo; }

// Immutable segment bundle. Shared across snapshots and with the writer;
// liveness and overlays belong to the snapshot, not these base files.
class SegmentCore {
public:
  struct Source {
    CatalogRange range;
    int32_t maxDoc = 0;
    std::vector<uint64_t> files;  // Expected size by file number, zero when absent.
  };

private:
  friend class PostingsReader;
  // Indexed by physical filenum and intentionally sparse. Segment finalize
  // folds buffered streams without renaming survivors, which keeps already
  // spilled filesystem/object-store keys stable.
  std::vector<std::shared_ptr<InputFile>> files;
  std::vector<InputStream> inputStreams;
  SegmentCatalog catalog;
  CatalogRange range;
  VersionRuns versions;

  const uint64_t segId;
  const uint64_t creatorEpoch;
  struct ChildSpace;
  std::vector<std::unique_ptr<ChildSpace>> children;
  PostingsReader root;

  SegmentCore(uint64_t segId, uint64_t creatorEpoch);
  bool initializeFromFiles(Directory& dir, const Source* source, bool missingFileOK, bool expectSynced);

public:
  static Source source(uint64_t id, uint64_t epoch, CatalogRange range, int32_t maxDoc,
                       std::span<const FileDescriptor> files);
  static std::shared_ptr<SegmentCore> create(Directory& dir, const api::SegmentInfo& segment,
                                            bool missingFileOK = false, bool expectSynced = false);
  static std::shared_ptr<SegmentCore> create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                            const Source& source, bool missingFileOK = false,
                                            bool expectSynced = false);
  static std::shared_ptr<SegmentCore> create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                            bool missingFileOK = false, bool expectSynced = false);
  explicit SegmentCore(Directory& dir, uint64_t segId, uint64_t creatorEpoch);
  ~SegmentCore();
  SegmentCore(const SegmentCore&) = delete;
  SegmentCore& operator=(const SegmentCore&) = delete;

  std::span<const SpaceDescriptor> spaces() const noexcept { return catalog.rowSpaces(); }
  const CatalogRange& catalogRange() const noexcept { return range; }
  std::optional<SpaceOrdinal> spaceOrdinal(std::string_view path) const noexcept;
  PostingsReader* spaceReader(SpaceOrdinal ordinal) noexcept;
  PostingsReader* spaceReader(std::string_view path) noexcept;
  const OwnershipShape* ownership(SpaceOrdinal child) const noexcept;
  const OwnershipShape* ownership(std::string_view path) const noexcept;

  std::string segmentName() const { return Postings::segmentKey(segId, creatorEpoch); }

  uint64_t id() const noexcept { return segId; }
  uint64_t epoch() const noexcept { return creatorEpoch; }
  PostingsReader& rootReader() noexcept { return root; }
  const PostingsReader& rootReader() const noexcept { return root; }
  const VersionRuns& versionRuns() const noexcept { return versions; }

  void forEachBaseFile(auto&& visit) const {
    for (uint32_t number = 0; number < files.size(); ++number) {
      if (const auto& file = files[number]) visit(number, (uint64_t)file->size());
    }
  }

  // Total bytes in the physical base-segment files this core owns.
  uint64_t sizeInBytes() const noexcept {
    uint64_t total = 0;
    forEachBaseFile([&](uint32_t, uint64_t bytes) {
      total = bytes > std::numeric_limits<uint64_t>::max() - total
          ? std::numeric_limits<uint64_t>::max() : total + bytes;
    });
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

  std::span<const char> slice(field_parts::Slice slice) {
    auto input = getInputStream(slice.location.filenum());
    return {input.ptr(slice.location.offset()), slice.bytes};
  }

  // return an InputStream positioned on the current location specified
  InputStream getInputStreamSeek(seg_location sloc) {
    InputStream is = getInputStream(sloc.filenum());
    is.seek(sloc.offset());
    return is;
  }

};

} // namespace luxir
