// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PostingsReader.h"
#include "SegmentCatalog.h"
#include "CatalogRange.h"
#include "VersionRuns.h"
#include <map>
#include <mutex>
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
  struct BaseFile {
    std::shared_ptr<InputFile> file;
    bool holdsFree = false;
  };
  std::vector<BaseFile> files;
  // Holds must outlive the views below and release before files.
  std::mutex holdsMutex;
  std::map<std::pair<uint64_t, uint64_t>, HeldRange> holds;
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
  std::span<const char> retain(InputFile& file, field_parts::Slice part);

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
      if (const auto& file = files[number].file) visit(number, (uint64_t)file->size());
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
    assert(files[fnum].file != nullptr);
    return files[fnum].file.get();
  }

  std::span<const char> slice(field_parts::Slice part) {
    auto number = part.location.filenum();
    assert(number < files.size());
    const auto& base = files[number];
    assert(base.file);
    if (base.holdsFree) return base.file->acquire({part.location.offset(), part.bytes}).bytes();
    return retain(*base.file, part);
  }

};

} // namespace luxir
