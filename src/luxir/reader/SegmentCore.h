// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PostingsReader.h"
#include "SegmentCatalog.h"
#include "CatalogRange.h"
#include "IdFilter.h"
#include "VersionRuns.h"
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include "luxir/store/Directory.h"
#include "luxir/util/SharedLazyCell.h"

namespace luxir {
class AcquireBatches;
class AcquireJoin;
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
    uint64_t size = 0;  // Read once at open; checked against the manifest when it records one.
    // RAM and mmap hand out views valid for the file's lifetime, so slice()
    // retains nothing for them and acquireSlices() requests nothing. Every
    // other path, the open included, is the same for every backend.
    bool holdsFree = false;
  };
  std::vector<BaseFile> files;
  // Holds must outlive the views below and release before files. A part is
  // held once; each part has one reader path, so its class does not vary.
  std::mutex holdsMutex;
  std::map<std::pair<uint64_t, uint64_t>, HeldRange> holds;
  SegmentCatalog catalog;
  CatalogRange range;
  // Built on first use. A build that throws publishes nothing, so the next
  // use retries it.
  mutable SharedLazyCell<VersionRuns> versions;
  bool everyHoldFree = true;  // every file hands out free views

  const uint64_t segId;
  const uint64_t creatorEpoch;
  struct ChildSpace;
  std::vector<std::unique_ptr<ChildSpace>> children;
  PostingsReader root;

  struct PendingOpen;

  SegmentCore(uint64_t segId, uint64_t creatorEpoch);
  bool openBaseFile(Directory& dir, std::string_view segment, uint32_t number, uint64_t expected,
                    bool missingFileOK, bool expectSynced, bool checkHeader);
  static void openPending(Directory& dir, std::span<PendingOpen> opens, bool missingFileOK, bool expectSynced);
  bool beginOpen(Directory& dir, PendingOpen& pending, bool missingFileOK, bool expectSynced, AcquireJoin& join);
  void finishOpen(const PendingOpen& pending);
  bool initializeFromFiles(Directory& dir, bool missingFileOK, bool expectSynced);
  void initializeSpaces();
  std::span<const char> retain(InputFile& file, field_parts::Slice part, ReadClass readClass);
  void releaseHold(field_parts::Slice part);
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
  // Opens together each segment whose entry in `cores` is null, and stores
  // its core there. Every file's header and every file 0's catalog go out in
  // one round of batches before any catalog is decoded. The segments fail in
  // order, as if opened one at a time: the first that fails throws, naming
  // the segment, and from the first with a missing file (when missingFileOK
  // is set) the entries stay null.
  static void open(Directory& dir, std::span<const api::SegmentInfo> segments,
                   std::span<std::shared_ptr<SegmentCore>> cores, bool missingFileOK = false,
                   bool expectSynced = false);
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

  // The root "id" field's membership filter, or none when the segment wrote no
  // filter part. Throws SegmentCorruptError when the part does not parse.
  std::optional<IdFilter> idFilter();

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

  // Acquires the parts in one batch per file, skipping parts already held,
  // keeps their holds as slice() does, and waits. Afterwards slice() of each
  // part makes no backend request: it returns the held bytes, or a free view
  // on a file with free holds, whose acquire costs nothing.
  void acquireSlices(std::span<const field_parts::Slice> parts, ReadClass readClass = ReadClass::DATA);
  // Issues the same batches on `join` without waiting, so batches of several
  // segments go out together; slice() makes no request once join.wait()
  // returns. A failure goes to `error` when given; see AcquireJoin::acquire.
  void acquireSlices(std::span<const field_parts::Slice> parts, ReadClass readClass, AcquireJoin& join,
                     std::exception_ptr* error = nullptr);
  // As acquireSlices(), with a read class per part, on any AcquireBatches:
  // still one batch per file.
  struct Part {
    field_parts::Slice slice;
    ReadClass readClass = ReadClass::DATA;
  };
  void acquireParts(std::span<const Part> parts, AcquireBatches& batches);
  // Whether slice() of `part` returns without an acquire: its file hands out
  // free views, or the segment holds the part.
  bool held(field_parts::Slice part);

  // A part of the segment, as listParts() lists it for a storage backend
  // that warms what reads will need.
  struct ListedPart {
    enum class Kind : uint8_t {
      CATALOG,     // file headers and the catalog, which an open reads
      NAVIGATION,  // what locates data: term routing data, column indexes, points directories, ...
      DATA,        // a field's data extents
    };
    Kind kind;
    ReadClass readClass;  // the class the readers acquire it with
    field_parts::Slice slice;
    std::string_view space;  // row space path, empty for the root
    std::string_view field;  // empty for a part of the segment rather than of a field
  };
  // Visits the file headers and the catalog, then each row space's
  // navigation (version runs, ownership columns) and its fields' navigation
  // parts, then the data parts of the fields `data` accepts. The parts are
  // the ranges the readers acquire, so a backend that holds them serves those
  // acquires; they may overlap (a run's docs within a field's postings).
  void listParts(const std::function<bool(std::string_view space, std::string_view field)>& data,
                 const std::function<void(const ListedPart&)>& visit);

private:
  // Issues the parts not yet held, one batch per file, through `issue`.
  template <class Issue>
  void issueParts(std::span<const Part> parts, Issue&& issue);

};

} // namespace luxir
