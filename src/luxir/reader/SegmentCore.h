// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PostingsReader.h"
#include "SegmentCatalog.h"
#include "CatalogRange.h"
#include "IdFilter.h"
#include "VersionRuns.h"
#include <atomic>
#include <map>
#include <mutex>
#include <span>
#include "luxir/store/Directory.h"
#include "luxir/util/SharedLazyCell.h"

namespace luxir {
class AcquireBatches;
class OwnershipShape;
namespace api { struct SegmentInfo; }

// Reads segments make beyond their open, which acquires only file headers and
// the catalog. Counted where a part is first held, so only on backends without
// free holds: RAM and mmap hand out free views and count nothing.
struct SegmentOpenCounters {
  std::atomic<uint64_t> segmentsOpened{0};      // segments with a first read beyond the catalog
  std::atomic<uint64_t> segmentBytesMapped{0};  // bytes of the parts those reads hold
};
inline SegmentOpenCounters segmentOpenCounters;

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
    uint64_t size = 0;  // The manifest's size when it records one, so reads need no size request.
    bool holdsFree = false;
  };
  std::vector<BaseFile> files;
  // Holds must outlive the views below and release before files. A part is
  // held once, under the read class of its first slice.
  std::mutex holdsMutex;
  std::map<std::pair<uint64_t, uint64_t>, HeldRange> holds;
  SegmentCatalog catalog;
  CatalogRange range;
  // Built on first use. A build that throws publishes nothing, so the next
  // use retries it.
  mutable SharedLazyCell<VersionRuns> versions;
  // Set when the open has read what it needs; later first holds are counted.
  bool opened = false;
  bool everyHoldFree = true;  // every file hands out free views
  std::atomic<bool> readBeyondCatalog{false};
  std::atomic<uint64_t> bytesBeyondCatalog{0};

  const uint64_t segId;
  const uint64_t creatorEpoch;
  struct ChildSpace;
  std::vector<std::unique_ptr<ChildSpace>> children;
  PostingsReader root;

  SegmentCore(uint64_t segId, uint64_t creatorEpoch);
  bool openBaseFile(Directory& dir, std::string_view segment, uint32_t number, uint64_t expected,
                    bool missingFileOK, bool expectSynced, bool checkHeader);
  bool beginOpen(Directory& dir, std::string_view segment, const Source& source, bool missingFileOK,
                 bool expectSynced, AcquireBatches& join);
  bool finishOpen(Directory& dir, std::string_view segment, const Source& source, bool missingFileOK,
                  bool expectSynced);
  bool initializeFromFiles(Directory& dir, bool missingFileOK, bool expectSynced);
  void initializeSpaces();
  std::span<const char> retain(InputFile& file, field_parts::Slice part, ReadClass readClass);
  void countFirstHold(uint64_t bytes) noexcept;
  const VersionRuns& buildVersionRuns() const;
  const OwnershipShape* buildOwnership(SpaceOrdinal child) const;

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
  // Opens the segments together: every file 0 acquires its header and catalog
  // in one batch, and all batches are issued before any catalog is decoded.
  // An entry is null where a file is missing and missingFileOK is set.
  static std::vector<std::shared_ptr<SegmentCore>> create(Directory& dir,
                                                          std::span<const api::SegmentInfo* const> segments,
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
  // The child space's ownership shape, null for the root or an absent space.
  // Built on first use, which may acquire its columns and throws
  // SegmentCorruptError when they are invalid.
  const OwnershipShape* ownership(SpaceOrdinal child) const;
  const OwnershipShape* ownership(std::string_view path) const;

  std::string segmentName() const { return Postings::segmentKey(segId, creatorEpoch); }

  uint64_t id() const noexcept { return segId; }
  uint64_t epoch() const noexcept { return creatorEpoch; }
  PostingsReader& rootReader() noexcept { return root; }
  const PostingsReader& rootReader() const noexcept { return root; }
  // Built on first use, which may acquire the version runs part.
  const VersionRuns& versionRuns() const {
    auto* built = versions.peek();
    return built ? *built : buildVersionRuns();
  }

  // Whether this core has held a part beyond the open's headers and catalog,
  // and the bytes of those parts; see SegmentOpenCounters.
  bool readPastCatalog() const noexcept { return readBeyondCatalog.load(std::memory_order_relaxed); }
  uint64_t bytesPastCatalog() const noexcept { return bytesBeyondCatalog.load(std::memory_order_relaxed); }

  // False when the root "id" dictionary cannot hold the term: the segment has
  // no id terms, the term is outside the dictionary's exact bounds, or the id
  // filter excludes it. The term is in the form the dictionary stores (fitted).
  bool mayContainId(std::string_view term) { return mayContainId(term, IdFilter::hash(term)); }
  // As above, with the term's IdFilter::hash computed once by the caller.
  bool mayContainId(std::string_view term, uint64_t hash);

  void forEachBaseFile(auto&& visit) const {
    for (uint32_t number = 0; number < files.size(); ++number) {
      if (files[number].file) visit(number, files[number].size);
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

  // Whether file `fnum` hands out free views (RAM, mmap): an acquire there
  // costs nothing, so its parts are read whole and never declared ahead.
  bool holdsFree(uint32_t fnum) const noexcept {
    assert(fnum < files.size() && files[fnum].file);
    return files[fnum].holdsFree;
  }
  // Whether every file of the segment does.
  bool holdsFree() const noexcept { return everyHoldFree; }

  InputFile* getFile(uint32_t fnum) {
    assert(fnum < files.size());
    assert(files[fnum].file != nullptr);
    return files[fnum].file.get();
  }

  // `readClass` tells a cache or backend whether the part is navigation
  // metadata or data; see ReadClass.
  std::span<const char> slice(field_parts::Slice part, ReadClass readClass = ReadClass::DATA) {
    auto number = part.location.filenum();
    assert(number < files.size());
    const auto& base = files[number];
    assert(base.file);
    if (base.holdsFree) {
      auto hold = base.file->acquire({part.location.offset(), part.bytes, readClass});
      return hold.bytes(); // Free views stay valid while the core keeps the file.
    }
    return retain(*base.file, part, readClass);
  }

  // Acquires the parts in one batch per file, keeps their holds as slice()
  // does, and returns their bytes in order.
  std::vector<std::span<const char>> slices(std::span<const field_parts::Slice> parts,
                                            ReadClass readClass = ReadClass::DATA);
  // Starts those batches on `join` without waiting, so batches of several
  // segments go out together. Once join.wait() returns, slice() of each part
  // returns its held bytes without an acquire.
  void acquireSlices(std::span<const field_parts::Slice> parts, ReadClass readClass, AcquireBatches& join);
  // As acquireSlices(), with a read class per part: still one batch per file.
  struct Part {
    field_parts::Slice slice;
    ReadClass readClass = ReadClass::DATA;
  };
  void acquireParts(std::span<const Part> parts, AcquireBatches& join);
  // Whether slice() of `part` returns without an acquire: its file hands out
  // free views, or the segment holds the part.
  bool held(field_parts::Slice part);

};

} // namespace luxir
