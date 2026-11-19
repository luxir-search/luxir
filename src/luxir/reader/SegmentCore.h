// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PostingsReader.h"
#include "VersionRuns.h"
#include <atomic>
#include <mutex>
#include <span>
#include "luxir/store/Directory.h"

namespace luxir {
class OwnershipShape;

// Immutable segment bundle. Shared across snapshots and with the writer;
// liveness and overlays belong to the snapshot, not these base files.
class SegmentCore {
  // Indexed by physical filenum and intentionally sparse. Segment finalize
  // folds buffered streams without renaming survivors, which keeps already
  // spilled filesystem/object-store keys stable.
  std::vector<std::shared_ptr<InputFile>> files;

  // The whole-file stream of each physical file. A file's stream is created on
  // first use: on a lazy file (an object store) that is the moment its bytes
  // are fetched, so files no query touches are never materialized. Files that
  // are already resident (memory, mmap) are published at open and cost one
  // acquire load per use.
  struct StreamSlot {
    std::atomic<bool> ready{false};
    std::mutex mutex;  // serializes the one materialization of this file only
    bool warned = false;  // a large lazy materialization was logged (under mutex)
    InputStream stream;
  };
  std::unique_ptr<StreamSlot[]> streams;
  InputStream materializeStream(uint32_t fnum, std::string_view caller);

  // The part of file 0 that describes the fields of every space: field
  // metadata, offsets tables, counts and the segment info. Opening a segment
  // reads exactly this tail of file 0 (plus the header of every file and the
  // request versions), so a lazy file fetches only those ranges. Offsets within
  // `catalog` count from catalogStart.
  InputStream catalog;
  int64_t catalogStart = 0;
  int64_t segInfoOffset;  // after this is segInfo, before this is the field index
  VersionRuns versions;

  const uint64_t segId;
  const uint64_t creatorEpoch;
  std::vector<SpaceDescriptor> spaceDescriptors;
  struct ChildSpace;
  std::vector<std::unique_ptr<ChildSpace>> children;
  PostingsReader root;

  void initializeSpace(PostingsReader& reader, const SpaceDescriptor& descriptor);

  SegmentCore(uint64_t segId, uint64_t creatorEpoch);
  bool initializeFromFiles(Directory& dir, bool missingFileOK, bool expectSynced);

public:
  static std::shared_ptr<SegmentCore> create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                            bool missingFileOK = false, bool expectSynced = false);
  explicit SegmentCore(Directory& dir, uint64_t segId, uint64_t creatorEpoch);
  ~SegmentCore();
  SegmentCore(const SegmentCore&) = delete;
  SegmentCore& operator=(const SegmentCore&) = delete;

  std::span<const SpaceDescriptor> spaces() const noexcept { return spaceDescriptors; }
  std::optional<SpaceOrdinal> spaceOrdinal(std::string_view path) const noexcept;
  PostingsReader* spaceReader(SpaceOrdinal ordinal) noexcept;
  PostingsReader* spaceReader(std::string_view path) noexcept;
  const OwnershipShape* ownership(SpaceOrdinal child) const noexcept;
  const OwnershipShape* ownership(std::string_view path) const noexcept;

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

  // A lazy file larger than this that is materialized whole (getInputStream)
  // is logged once per file, so the readers that still take whole files on
  // remote storage are visible. Set from reader.max-materialize-bytes; zero
  // logs none.
  static inline std::atomic<uint64_t> maxMaterializeBytes{64ULL << 20};
  // Lazy files over maxMaterializeBytes materialized so far, node-wide.
  static inline std::atomic<int64_t> largeMaterializations{0};

  // The data file's name, for messages.
  std::string fileName(uint32_t fnum) const;

  InputFile* getFile(uint32_t fnum) {
    assert(fnum < files.size());
    assert(files[fnum] != nullptr);
    return files[fnum].get();
  }

  // Bulk readers use this: it hands out the whole file, so on a lazy file the
  // first call materializes it. Open-time reads go through getFile()->rangeStream().
  // `caller` (a field name, say) names the reader in the large-materialization warning.
  InputStream getInputStream(uint32_t fnum, std::string_view caller = {}) {
    assert(fnum < files.size());
    assert(files[fnum] != nullptr);
    StreamSlot& slot = streams[fnum];
    if (!slot.ready.load(std::memory_order_acquire)) [[unlikely]] return materializeStream(fnum, caller);
    return slot.stream;
  }

  // The whole-file stream when it exists already (a resident file, or a lazy
  // one some reader materialized); never materializes.
  std::optional<InputStream> residentStream(uint32_t fnum) const {
    assert(fnum < files.size());
    assert(files[fnum] != nullptr);
    const StreamSlot& slot = streams[fnum];
    if (!slot.ready.load(std::memory_order_acquire)) return std::nullopt;
    return slot.stream;
  }

  // Field metadata of all spaces, resident since open; see `catalog`.
  const InputStream& fieldCatalog() const noexcept { return catalog; }
  int64_t fieldCatalogStart() const noexcept { return catalogStart; }

  // return an InputStream positioned on the current location specified
  InputStream getInputStreamSeek(seg_location sloc, std::string_view caller = {}) {
    InputStream is = getInputStream(sloc.filenum(), caller);
    is.seek(sloc.offset());
    return is;
  }

  void checkLocation(seg_location loc, uint64_t length = 0) const;
  void checkFieldLocations(const SegFieldInfo& field) const;
};

} // namespace luxir
