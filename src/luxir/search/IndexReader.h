// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "DocSet.h"
#include "Similarity.h"
#include "luxir/index/AuxInfo.h"
#include "luxir/index/SegmentSummary.h"
#include "luxir/query/SegmentMask.h"
#include "luxir/reader/AuxReader.h"
#include "luxir/reader/Hotcache.h"
#include "luxir/reader/TermMembership.h"
#include "luxir/reader/SegmentCore.h"
#include "luxir/reader/RankLiveBitmap.h"
#include "luxir/store/HeldFiles.h"
#include "luxir/util/Metrics.h"
#include "luxir/util/screaming.h"
#include "luxir/util/Once.h"
#include "luxir/util/SharedLazyCell.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define IREADER_DEBUG LOG_TRACE
// #define IREADER_DEBUG LOG_DEBUG

namespace luxir {
class FieldType;
class FilterCache;
class OrdMap;
class Schema;
struct CommitSnapshot;

/// LiveDocs holds the live document bitmap for a segment
class LiveDocs {
private:
  BitDocSet docSet;                                   // The actual bitset (memory mapped)
  std::shared_ptr<InputFile> deleteFile;              // Keep file alive for memory mapping
  std::unique_ptr<uint64_t[]> ownedWords;
  size_t derivedBytes = 0;
  mutable std::mutex rankLiveMutex;
  mutable std::vector<std::pair<std::string, std::unique_ptr<const RankLiveBitmap>>> rankLiveViews;

public:
  LiveDocs() : docSet(FixedBitSet(nullptr, 0)) {}

  LiveDocs(uint64_t* mappedMemory, int32_t maxDocCount, 
           std::shared_ptr<InputFile>&& file, int32_t numLive)
      : docSet(FixedBitSet(mappedMemory, maxDocCount), numLive),
        deleteFile(std::move(file)) {}

  LiveDocs(std::unique_ptr<uint64_t[]> words, int32_t maxDoc, int32_t numLive);
  ~LiveDocs();

  // Static factory method to create LiveDocs from delete bitmap file.  Do not call this if liveGen is 0 (no
  // deletes for this segment).  A nullptr is returned in the case that there are deletions but we couldn't
  // find the delete file (this is not necessarily an error, the index could have changed already).
  // If missingFileOK is false, missing files will throw exceptions instead of returning nullptr.
  static std::shared_ptr<LiveDocs> create(Directory& dir, uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen, uint64_t liveEpoch, int32_t maxDoc, bool missingFileOK = true, bool expectSynced = false);
  
  // Field names identify immutable physical columns across ANN overlay replacements.
  // Callers must retain this LiveDocs and alias its shared_ptr to the returned
  // record. The record must never own this LiveDocs, which would form a cycle.
  // Holding the lock during build prevents concurrent first callers folding twice.
  template <typename Build>
  const RankLiveBitmap& rankLiveBitmap(std::string_view field, Build&& build) const {
    std::lock_guard lock(rankLiveMutex);
    for (const auto& [name, view] : rankLiveViews) {
      if (name == field) return *view;
    }
    auto made = std::make_unique<const RankLiveBitmap>(build());
    rankLiveViews.emplace_back(field, std::move(made));
    return *rankLiveViews.back().second;
  }

  // Get the underlying FixedBitSet for direct access
  const screaming::FixedBitSet& bitset() const {
    return docSet.bits();
  }

  // Get the underlying BitDocSet for direct access
  BitDocSet& docset() {
    return docSet;
  }

  int32_t numDeletes() const {
    return size() - numLive();
  }
  
  int32_t numLive() const {
    return docSet.cachedCard();
  }
  
  int32_t size() const {
    return docSet.bits().size();
  }
};

// One lazily materialized part of a segment: the files it needs, secured by
// open handles when the snapshot opened, and the value built from them on
// first need. Shared by every snapshot (and the writer) holding the same
// files, opened or not; once built the handles close, since the mappings
// keep the bytes. A build that fails on an I/O or resource error (an
// ApiError such as a failed mmap, std::system_error, std::bad_alloc) throws
// and a later call retries. One that fails on the bytes themselves (a format
// or checksum error, EIO, or a held file absent at decode) marks the part
// unreadable: it is logged at error level once, its handles close, every
// later call rethrows the same error without rebuilding, and the part's
// files are reported to its owner's UnreadableHook as suspects, so they are
// recorded and repaired the way a scrub mismatch is.
//
// The hook runs on the thread that found the damage, possibly a writer's
// under its locks: it must not throw, block, or fail the collection inline.
using UnreadableHook = std::function<void(std::vector<std::string> files, std::string reason)>;

template <class Value>
class SegmentPart {
protected:
  SharedLazyCell<Value, false> cell;
  std::unique_ptr<HeldFiles> files;
  // Guards `files` between the build and declareReads, which another request
  // may run while this one builds.
  std::mutex filesMutex;
  std::shared_ptr<Metrics> metrics;
  std::shared_ptr<const UnreadableHook> onUnreadable;  // null: nobody to tell
  // Set once by the (serialized) build that found the bytes unreadable;
  // `failure` is written before `unreadable` is released.
  std::atomic<bool> unreadable{false};
  std::string failure;

  SegmentPart(std::unique_ptr<HeldFiles> files, std::shared_ptr<Metrics> metrics,
              std::shared_ptr<const UnreadableHook> onUnreadable)
      : files(std::move(files)), metrics(std::move(metrics)), onUnreadable(std::move(onUnreadable)) {}
  explicit SegmentPart(std::shared_ptr<Value> opened) : cell(std::move(opened)) {}

  // Builds through the held files; `what` names the part in errors.
  template <class Open>
  std::shared_ptr<Value> getOrOpen(std::string_view what, Open&& open);
  // Adds the reads a build of the part makes through `held`, for a fetch
  // ahead of it, under filesMutex. Every held file whole by default: a part
  // whose open reads only regions of large files declares those.
  virtual void declareOpen(HeldFiles& held, ReadSet& reads) { held.declareReads(reads); }

public:
  virtual ~SegmentPart() = default;
  // The built value, or null when it is not built yet; never builds.
  std::shared_ptr<Value> peek() const noexcept { return cell.peek(); }
  // Whether a build found the part's bytes unreadable; it never retries.
  bool isUnreadable() const noexcept { return unreadable.load(std::memory_order_acquire); }
  // peek() without a reference count; valid while the part lives.
  Value* peekRaw() const noexcept { return cell.peekRaw(); }
  // Adds the reads a build would make, for a fetch ahead of it; nothing once
  // built or found unreadable.
  void declareReads(ReadSet& reads);
};

// A segment's base data: its SegmentCore over the segment files, with every
// row space. The manifest's row counts describe it before it is open, and
// the open core must agree with them.
class SegmentBase : public SegmentPart<SegmentCore> {
  // The segment's hotcache, once a declare read it; empty before that, or
  // when the segment has none or it is unreadable. Under filesMutex.
  std::optional<Hotcache> hotcache;
  bool hotcacheRead = false;

protected:
  // The hotcache's ranges when the segment has one, fetched in a round of its
  // own first; otherwise each data file's header and file 0's tail.
  void declareOpen(HeldFiles& held, ReadSet& reads) override;

public:
  using SpaceRows = std::vector<std::pair<std::string, int32_t>>;
  const uint64_t segId;
  const uint64_t creatorEpoch;
  const int32_t maxDoc;
  // Each child row space and its row count, sorted by path as in the core's
  // catalog, whose ordinal of the i-th entry is i + 1.
  const SpaceRows spaces;
  SegmentBase(uint64_t segId, uint64_t creatorEpoch, int32_t maxDoc, SpaceRows spaces,
              std::unique_ptr<HeldFiles> files, std::shared_ptr<Metrics> metrics,
              std::shared_ptr<const UnreadableHook> onUnreadable)
      : SegmentPart(std::move(files), std::move(metrics), std::move(onUnreadable)), segId(segId),
        creatorEpoch(creatorEpoch), maxDoc(maxDoc), spaces(std::move(spaces)) {}
  SegmentBase(uint64_t segId, uint64_t creatorEpoch, int32_t maxDoc, SpaceRows spaces,
              std::shared_ptr<SegmentCore> opened)
      : SegmentPart(std::move(opened)), segId(segId), creatorEpoch(creatorEpoch), maxDoc(maxDoc),
        spaces(std::move(spaces)) {}
  std::shared_ptr<SegmentCore> get();
  // The space `path`: its ordinal and row count, or nullopt when the segment
  // has no rows there. Never opens.
  std::optional<std::pair<SpaceOrdinal, int32_t>> space(std::string_view path) const noexcept;
};

// The live documents of one delete generation.
class SegmentLive : public SegmentPart<LiveDocs> {
public:
  const uint64_t segId;
  const uint64_t creatorEpoch;
  const uint64_t liveGen;
  const uint64_t liveEpoch;
  const int32_t maxDoc;
  const int32_t numLive;
  SegmentLive(uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen, uint64_t liveEpoch, int32_t maxDoc,
              int32_t numLive, std::unique_ptr<HeldFiles> files, std::shared_ptr<Metrics> metrics,
              std::shared_ptr<const UnreadableHook> onUnreadable)
      : SegmentPart(std::move(files), std::move(metrics), std::move(onUnreadable)), segId(segId),
        creatorEpoch(creatorEpoch), liveGen(liveGen), liveEpoch(liveEpoch), maxDoc(maxDoc), numLive(numLive) {}
  // A part born ready with already loaded live docs.
  SegmentLive(uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen, uint64_t liveEpoch,
              std::shared_ptr<LiveDocs> opened)
      : SegmentPart(opened), segId(segId), creatorEpoch(creatorEpoch), liveGen(liveGen), liveEpoch(liveEpoch),
        maxDoc(opened->size()), numLive(opened->numLive()) {}
  std::shared_ptr<LiveDocs> get();
};

// A segment overlay of a known kind (a vector index, the test overlay).
class SegmentOverlay : public SegmentPart<AuxReader> {
  // A vector overlay's open ranges, once a declare read its footer; empty
  // before that. Under filesMutex.
  std::optional<std::vector<ByteRange>> openRanges;

protected:
  // A vector overlay (large: its inverted lists) declares its footer, fetched
  // in a round of its own first, then the header and list sizes the footer
  // locates. Other overlays are small and declare their files whole.
  void declareOpen(HeldFiles& held, ReadSet& reads) override;

public:
  const AuxInfo info;
  const SegmentOverlayPolicy policy;
  SegmentOverlay(AuxInfo info, SegmentOverlayPolicy policy, std::unique_ptr<HeldFiles> files,
                 std::shared_ptr<Metrics> metrics, std::shared_ptr<const UnreadableHook> onUnreadable)
      : SegmentPart(std::move(files), std::move(metrics), std::move(onUnreadable)), info(std::move(info)),
        policy(policy) {}
  std::shared_ptr<AuxReader> get();
};

// One lazy value for a path, bundle and root live generation. Shared by leaves
// of successive snapshots; constructing a view does not construct its bitmap,
// nor materialize the segment it derives from.
class ChildLiveDocs {
  std::shared_ptr<SegmentBase> base;
  std::shared_ptr<SegmentLive> root;
  std::string path;
  std::atomic<int32_t> knownCard{-1};
  SharedLazyCell<LiveDocs> value;

public:
  ChildLiveDocs(std::shared_ptr<SegmentBase> base, std::shared_ptr<SegmentLive> root, std::string path)
      : base(std::move(base)), root(std::move(root)), path(std::move(path)) {}
  std::shared_ptr<LiveDocs> get();
  int32_t knownCount() const noexcept { return knownCard.load(std::memory_order_acquire); }
};

  // A snapshot's view of one segment in one row space. Opening the snapshot
  // reads only the manifest and the segment's summary and secures its files
  // by handle; the core, live docs and overlays are parts of the segment's
  // bundle, shared by every space's view of it, and materialize later,
  // together at the search's batched point (IndexReader::materialize) or on
  // first use. Row counts come from the manifest, so sizing a view never
  // opens anything.
  class Segment {
    const std::shared_ptr<SegmentBase> sharedBase;
    const std::shared_ptr<SegmentLive> sharedLive;          // root space only
    const std::shared_ptr<ChildLiveDocs> childLiveDocs;     // child spaces only
    const std::vector<std::shared_ptr<SegmentOverlay>> sharedOverlays;
    const std::shared_ptr<const SegmentSummary> sharedSummary;  // root space only
    const SpaceOrdinal space;  // UINT32_MAX when the segment has no rows here
    const int32_t rowCount;
    const int32_t liveCount;  // exact unless childLiveDocs decides it
  public:
    friend class IndexReader;

    struct SegmentInfo {
      uint64_t seg_id;          // Unique identifier for the segment
      uint64_t live_gen;        // What deletes version to use for the segment (0 if no deletes)
      uint64_t min_version;     // Minimum update version in this segment
      uint64_t max_version;     // Maximum update version in this segment
      uint64_t commit_time;     // first time this segment was committed as part of the index
      int32_t  max_doc;         // Number of documents in this segment, deleted ones included
      int32_t  live_docs;       // Number of live documents in this segment
    };

    const SegmentInfo segInfo;    // metadata read from the index info file about the segment
    const int64_t base;           // global index (ordinal/rank) of the first document in this segment with respect to the list of segments
    const int32_t ord;            // index of this segment in the list of segments

    static constexpr SpaceOrdinal NO_SPACE = (SpaceOrdinal)UINT32_MAX;

    Segment(std::shared_ptr<SegmentBase> data, std::shared_ptr<SegmentLive> live,
            std::vector<std::shared_ptr<SegmentOverlay>> overlays,
            SegmentInfo segInfo, int64_t docBase, int ord, SpaceOrdinal space, int32_t rowCount,
            int32_t liveCount, std::shared_ptr<ChildLiveDocs> childLiveDocs = {},
            std::shared_ptr<const SegmentSummary> summary = {})
            :  sharedBase(std::move(data)), sharedLive(std::move(live)),
                childLiveDocs(std::move(childLiveDocs)), sharedOverlays(std::move(overlays)),
                sharedSummary(std::move(summary)), space(space), rowCount(rowCount),
                liveCount(liveCount), segInfo(segInfo), base(docBase), ord(ord) {
      assert(!sharedLive || space == SpaceOrdinal::ROOT);
    }

    // The segment's summary sidecar, loaded when the snapshot opened; null
    // when the segment has none readable, which proves nothing about it.
    const SegmentSummary* summary() const noexcept { return sharedSummary.get(); }

    // This space's reader; materializes the core on first use.
    PostingsReader& postingsReader() const {
      assert(hasSpace());
      return *segmentCore().spaceReader(space);
    }

    // This space's reader when the core is materialized, else null; never opens.
    PostingsReader* postingsReaderIfOpen() const noexcept {
      auto* core = sharedBase->peekRaw();
      return core && hasSpace() ? core->spaceReader(space) : nullptr;
    }

    bool hasSpace() const noexcept { return space != NO_SPACE; }

    SpaceOrdinal spaceOrdinal() const noexcept { return space; }

    // Materializes the core on first use.
    SegmentCore& segmentCore() const {
      if (auto* core = sharedBase->peekRaw()) return *core;
      return *sharedBase->get();
    }

    // Pins the immutable bundle and all of its space readers; materializes it.
    std::shared_ptr<SegmentCore> segmentCoreShared() const {
      return sharedBase->get();
    }

    // The bundle's core part, opened or not.
    const std::shared_ptr<SegmentBase>& basePart() const noexcept { return sharedBase; }

    bool isOpen() const noexcept { return sharedBase->peekRaw() != nullptr; }
    // Whether the core, root live docs and every overlay opened with the
    // segment are materialized.
    bool isMaterialized() const noexcept;
    // Materializes the core, root live docs and every overlay opened with the segment.
    void materialize() const;
    // Adds the reads materialize() would make, for a fetch ahead of it.
    void declareReads(ReadSet& reads) const;

    // Whether this space has deleted rows. For the root, from the manifest;
    // a child space derives its live rows, materializing the segment.
    bool hasDeletes() const { return numLive() != rowCount; }

    // Metadata probes must not construct a derived live view. Until it has
    // been used, a child count is an interval rather than an exact count.
    std::pair<int32_t, int32_t> liveCountBounds() const noexcept {
      int32_t count = childLiveDocs ? childLiveDocs->knownCount() : liveCount;
      return count < 0 ? std::pair{0, rowCount} : std::pair{count, count};
    }

    bool mayHaveDeletes() const noexcept { return liveCountBounds().first != rowCount; }

    // returns null if all docs are live (no deletes); materializes them on first use
    LiveDocs* liveDocs() const {
      if (childLiveDocs) return childLiveDocs->get().get();
      if (!sharedLive) return nullptr;
      if (auto* live = sharedLive->peekRaw()) return live;
      return sharedLive->get().get();
    }

    // The segment's live docset, or null when there are no deletes.
    BitDocSet* liveDocSet() const {
      auto* live = liveDocs();
      return live ? &live->docset() : nullptr;
    }

    // shared form of liveDocs(), for callers that cache views into its mapped bits
    std::shared_ptr<LiveDocs> liveDocsShared() const {
      if (childLiveDocs) return childLiveDocs->get();
      return sharedLive ? sharedLive->get() : nullptr;
    }

    // Every overlay of a known kind opened with the segment, materialized.
    std::vector<std::shared_ptr<AuxReader>> auxReaders() const;

    // Whether an overlay of this segment can prove terms absent.
    bool hasTermMembership() const noexcept;
    // Whether an overlay proves `term` (dictionary form) absent from `field`.
    // Fills `why` with the proof's label when it does. Never throws: any
    // failure proves nothing.
    bool membershipProvesAbsent(std::string_view field, std::string_view term, std::string* why = nullptr) const noexcept;
    // Adds the byte ranges those probes will read.
    void declareMembershipProbes(std::string_view field, std::string_view term, ReadSet& reads) const noexcept;

    // The overlay `name`, materialized on first use; null when absent.
    std::shared_ptr<AuxReader> getAuxReader(std::string_view name) const;

    int32_t maxDoc() const noexcept {
      return rowCount;
    }

    // Number of deleted rows in this space.
    int32_t numDeletes() const {
      return maxDoc() - numLive();
    }

    // Number of live rows in this space; a child space derives it.
    int32_t numLive() const {
      if (!childLiveDocs) return liveCount;
      auto* live = liveDocs();
      return live ? live->numLive() : rowCount;
    }
  };





/// Thread-safe scoped view of one space in an immutable snapshot.
class IndexReader : public std::enable_shared_from_this<IndexReader> {
  struct FieldRecord;
  struct HeaderTotals;

public:
  using Segment = ::luxir::Segment;

  // A borrowed view, valid while its reader lives. Null means absent.
  // at() lazily resolves ordinals and headers in this core's segments,
  // materializing a segment only where the field may be present. stats()
  // and flagsUnion() total every segment's header, deleted docs included,
  // from complete summaries where segments have them, so they open nothing.
  class FieldSegments {
    friend class IndexReader;
    const FieldRecord* record = nullptr;
    const Segment* segments = nullptr;
    FieldSegments(const FieldRecord* record, const Segment* segments)
        : record(record), segments(segments) {}
    int32_t ordinalAt(size_t segment) const;
    const HeaderTotals& totals() const;

  public:
    FieldSegments() = default;
    const PostingsReader::Field* at(size_t segment) const;
    const PostingsReader::Field* at(const Segment& segment) const {
      assert(!record || (segment.ord >= 0 && (size_t)segment.ord < size()
                         && &segments[segment.ord] == &segment));
      return at((size_t)segment.ord);
    }
    std::string_view name() const noexcept;
    bool exists() const noexcept;
    size_t size() const noexcept;
    // False when the segment provably lacks the field (its summary, or an
    // earlier probe); true when it has it or that is not known yet. Never opens.
    bool presentIn(size_t segment) const noexcept;
    const Similarity::FieldStats& stats() const;
    // OR of the segment headers' flags, such as segment-format markers.
    FieldType::flag_type flagsUnion() const;
    bool operator==(const FieldSegments&) const = default;
  };

  struct ProjectableField {
    std::string_view name;
    bool column = false;
    std::vector<std::string_view> storedResources;
  };

  // One retrievable representation: a logical root (its primary's stored
  // source or own column) or a derived variant (its own column), named by
  // its physical name.
  struct RetrievableField {
    std::string_view name;
    FieldType* type;
    bool derived;
  };

private:
  friend class ReaderManager;
  friend class FilterCache;

  struct HeaderTotals {
    Similarity::FieldStats stats;
    FieldType::flag_type flags = 0;
    FieldType::Type type = FieldType::NONE;
    uint32_t typeFlags = 0;
  };

  // Positive names, header totals and OrdMaps shared by segment sets on one path
  // with the same core generation. The record owns its name and lazily caches each ordinal;
  // it holds no Segment or reader, so views can use different open readers.
  struct FieldRecord {
    static constexpr int32_t UNRESOLVED = -2;
    std::string name;
    size_t hash;
    size_t numSegments;
    std::unique_ptr<std::atomic<int32_t>[]> ordinals;
    mutable std::atomic<HeaderTotals*> totals{nullptr};
    mutable SharedLazyCell<OrdMap, true> ordMap;
    size_t bytes;

    FieldRecord(std::string_view name, size_t hash, std::span<const int32_t> initialOrdinals);
    ~FieldRecord();
  };

  // Records by name. Insert-only, so a lookup takes no lock and writes no
  // shared memory. Inserts serialize; one that grows the table publishes a
  // doubled copy, and replaced tables live as long as the records because a
  // lookup may still be probing one.
  class FieldRecords {
    struct Table {
      size_t mask;
      std::unique_ptr<std::atomic<const FieldRecord*>[]> slots;
      explicit Table(size_t capacity);
      const FieldRecord* find(std::string_view name, size_t hash) const noexcept;
      void place(const FieldRecord* record) noexcept;
    };
    std::atomic<const Table*> current{nullptr};
    std::mutex insertMutex;
    std::vector<std::unique_ptr<Table>> tables;
    std::vector<std::unique_ptr<FieldRecord>> records;

  public:
    static constexpr size_t BYTES_PER_RECORD =
        sizeof(std::unique_ptr<FieldRecord>) + sizeof(std::atomic<const FieldRecord*>);

    const FieldRecord* find(std::string_view name, size_t hash) const noexcept {
      const Table* table = current.load(std::memory_order_acquire);
      return table ? table->find(name, hash) : nullptr;
    }
    // Returns the record kept for its name: this one, or one inserted first.
    const FieldRecord* insert(std::unique_ptr<FieldRecord> record);
  };

  // Per-space physical state can be shared by schema-only publications.
  // Its lazy catalogs and maps never retain a reader or schema.
  struct SpaceState {
    std::string path;
    std::vector<Segment> segs;
    OnceFlag fieldCatalogsOnce;
    std::vector<std::string_view> physicalNames;
    std::vector<ProjectableField> projectable;
    std::shared_ptr<FieldRecords> fields;
    int64_t totalMaxDoc = 0;
    int64_t livedocs = 0;
    OnceFlag liveOnce;

    void buildFieldCatalogs();
    std::span<const ProjectableField> projectableFields() {
      buildFieldCatalogs();
      return projectable;
    }
    std::span<const std::string_view> physicalFieldNames() {
      buildFieldCatalogs();
      return physicalNames;
    }
  };

  struct PhysicalCore {
    struct ChildLives {
      std::mutex mutex;
      std::map<std::string, std::shared_ptr<ChildLiveDocs>, std::less<>> paths;
    };
    // One physical segment's parts, materialized or not, shared by every
    // space's view of it.
    struct Bundle {
      std::shared_ptr<SegmentBase> core;
      std::shared_ptr<SegmentLive> live;
      std::vector<std::shared_ptr<SegmentOverlay>> aux;
      Segment::SegmentInfo info;
      int32_t ord;
      std::shared_ptr<ChildLives> childLives;
      // The root space's summary sidecar; null when the segment has none.
      std::shared_ptr<const SegmentSummary> summary;
    };
    std::vector<Bundle> bundles;
    std::vector<std::shared_ptr<AuxReader>> auxReadersList;
    uint64_t coreGeneration = 0;
    // The manifest's collection summary when it is complete, else null.
    std::shared_ptr<const SegmentSummary> collectionSummary;
    // Commit time of the publication that produced this physical state. A
    // later publication that reuses the core (a schema-only one) keeps it.
    uint64_t commitTime = 0;
    std::shared_ptr<SpaceState> root;
    std::mutex spacesMutex;
    std::map<std::string, std::shared_ptr<SpaceState>, std::less<>> spaces;
    // A segment's physical identity, which decides what a reopen reuses: the
    // base files its SegmentCore maps, the live-docs file of this
    // snapshot's generation (none without deletes), and each overlay's files
    // (aux readers, reused by their own gen rule). Within an incarnation an
    // equal (segId, files) is the same mapped content, as is an equal
    // liveDocs; descriptors carry size and digest, so a segment id reused by
    // diverged history never matches.
    struct SegmentFiles {
      uint64_t segId;
      std::vector<FileDescriptor> files;
      std::vector<FileDescriptor> liveDocs;
      std::vector<std::vector<FileDescriptor>> overlays;
      bool operator==(const SegmentFiles&) const = default;
    };
    std::vector<SegmentFiles> segmentFiles;
    std::vector<std::vector<FileDescriptor>> auxFiles;
    std::string incarnation;
    // (segId, ord) of each segment, by segId.
    std::vector<std::pair<uint64_t, int32_t>> segOrds;

    // The bundle `segId`, or null.
    const Bundle* bundle(uint64_t segId) const;
    std::shared_ptr<SpaceState> space(std::string_view path);
  };

  struct OpenedSnapshot {
    std::shared_ptr<PhysicalCore> core;
    std::shared_ptr<Schema> schema;
    uint64_t commitTime = 0;
    uint64_t indexGen = 0;
  };

  struct Snapshot {
    std::shared_ptr<PhysicalCore> core;
    std::shared_ptr<Schema> schema;
    std::shared_ptr<FilterCache> filterCache;
    uint64_t commitTime;
    uint64_t indexGen;
    std::mutex viewsMutex;
    std::map<std::string, std::weak_ptr<IndexReader>, std::less<>> views;
    // The core's collection summary, unless a field it records persists a
    // type this snapshot's schema does not match (see collectionSummary()).
    const SegmentSummary* provingSummary = nullptr;

    Snapshot(OpenedSnapshot opened, std::shared_ptr<FilterCache> filterCache)
        : core(std::move(opened.core)), schema(std::move(opened.schema)),
          filterCache(std::move(filterCache)), commitTime(opened.commitTime), indexGen(opened.indexGen),
          provingSummary(provingSummaryOf(*core, schema.get())) {}
  };
  static const SegmentSummary* provingSummaryOf(const PhysicalCore& core, const Schema* schema);
  const std::shared_ptr<Snapshot> snapshot;
  const std::shared_ptr<SpaceState> space;
  OnceFlag retrievableOnce;
  std::vector<RetrievableField> retrievable;
  OnceFlag searchableOnce;
  std::vector<std::string> searchable;

  // Opens the snapshot `manifest` (the newest durable root when null),
  // securing every file it needs by an open handle inside the retry loop: a
  // file a newer root retired before its handle opened retries against that
  // root, and once held no deletion can take it. Nothing else is read but the
  // manifest and the segment summaries. Each segment's core comes from
  // `previous` when its files are identical (opened or not), unless
  // `published` (the publisher's open cores, by manifest segment order)
  // hands over a different open one; live docs and overlays likewise come
  // from `previous` when their files are identical. `materialize` opens every
  // segment before returning.
  static OpenedSnapshot openSnapshot(Directory& dir, const IndexReader* previous,
                               std::shared_ptr<Schema> schema,
                               std::shared_ptr<const std::vector<std::byte>> manifest,
                               std::span<const std::weak_ptr<SegmentCore>> published,
                               const std::shared_ptr<Metrics>& metrics, bool materialize,
                               const std::shared_ptr<const UnreadableHook>& onUnreadable);

  IndexReader(OpenedSnapshot opened, std::shared_ptr<FilterCache> filterCache);
  IndexReader(std::shared_ptr<Snapshot> snapshot, std::shared_ptr<SpaceState> space);

public:
  std::string_view path() const noexcept { return space->path; }
  std::shared_ptr<IndexReader> view(std::string_view path);

  const std::shared_ptr<Schema>& schema() const noexcept {
    assert(snapshot->schema);
    return snapshot->schema;
  }

  // The time in microseconds when this version of the index was committed.  Guaranteed to be strictly increasing
  // with new versions of the index.
  uint64_t commitTime() const noexcept {
    return snapshot->commitTime;
  }

  uint64_t commitId() const noexcept { return snapshot->indexGen; }

  // The commit time of the publication that produced this reader's physical
  // state (segments, live docs, overlays).  A reader that reuses the previous
  // core, as a schema-only publication does, reports the previous value, so
  // reader-stable caches can build and hit values after that publication.
  // Filter keys still include schemaGen, so new-schema requests build new values.
  uint64_t coreCommitTime() const noexcept {
    return snapshot->core->commitTime;
  }
  std::string_view incarnation() const noexcept { return snapshot->core->incarnation; }

  std::span<Segment> segments() noexcept {
    return space->segs;
  }

  // Aux readers for entries in the parsed IndexInfo.aux_indexes (in the same
  // order).  Unknown-kind entries are omitted, so this list may be shorter
  // than IndexInfo.aux_indexes.
  std::span<const std::shared_ptr<AuxReader>> auxReaders() const noexcept {
    return path().empty() ? std::span<const std::shared_ptr<AuxReader>>(snapshot->core->auxReadersList)
                          : std::span<const std::shared_ptr<AuxReader>>{};
  }

  // Returns the INDEX-LEVEL aux reader with the given name, or nullptr.
  // Names are unique across an index for index-level entries.  Segment
  // overlays (per-segment vector indexes etc.) are NOT in this registry -
  // their names repeat across segments; use Segment::getAuxReader().
  // TODO: replace this O(n) scan with a name -> AuxReader hash map populated
  // at IndexReader construction.  Fine for v1 (<= a handful of aux entries
  // per shard); revisit if we add many cheap aux kinds (autocomplete,
  // spell-check) so the per-query lookup count grows.
  std::shared_ptr<AuxReader> getAuxReader(std::string_view name) const {
    for (const auto& r : auxReaders()) {
      if (r->getName() == name) return r;
    }
    return nullptr;
  }

  int64_t maxDoc() const noexcept {
    return space->totalMaxDoc;
  }

  int64_t liveDocs() const {
    if (!space->path.empty()) {
      callOnce(space->liveOnce, [&] {
        space->livedocs = 0;
        for (const auto& segment : space->segs) space->livedocs += segment.numLive();
      });
    }
    return space->livedocs;
  }
  
  // Get the core generation for this index reader
  // This can be used as a cache key for structures that depend on segments but don't care about deletes.
  uint64_t coreGen() const noexcept {
    return snapshot->core->coreGeneration;
  }

  // The collection summary of this snapshot's manifest (the union of every
  // segment's summary), or null when the manifest has no complete one: an
  // incomplete aggregate proves nothing. Also null when the schema types a
  // recorded field differently from its persisted type: a proof answers
  // without weights, so it would hide the field_type_mismatch that field()
  // raises for requests using that field; those requests take the
  // per-segment path instead.
  // Null for a child view: summaries describe the root space.
  const SegmentSummary* collectionSummary() const noexcept {
    return space == snapshot->core->root ? snapshot->provingSummary : nullptr;
  }

  // Validate this schema's type against the field's persisted segment types
  // on access. Unused fields never prevent opening or using a reader.
  FieldSegments field(std::string_view name) const;

  std::shared_ptr<OrdMap> getOrdMap(std::string_view name);

  // This reader's segment `segId`, or null.
  const Segment* findSegment(uint64_t segId) const {
    auto* bundle = snapshot->core->bundle(segId);
    return bundle ? &space->segs[(size_t)bundle->ord] : nullptr;
  }

  FilterCache* filterCache() const noexcept {
    return snapshot->filterCache.get();
  }

  // Fields some segment of this reader can project into a
  // DocList: column-backed fields (COLUMN_STORED, any non-BIN type) and the
  // fields held by each stored-fields resource.  Physical and
  // schema-independent - read from segment metadata, so dynamic
  // (suffix-template) fields appear under their concrete names.  Sorted and
  // unique by name, with column presence and the resources storing that name
  // retained for schema-specific source selection. Built once on first use,
  // which materializes every segment with rows in this space (stored
  // resources list their fields only in segment data), and immutable
  // afterwards (segments never
  // change under a reader).  The views point into segment metadata that this
  // reader's segment cores keep mapped, so they are valid for as long as the
  // caller holds the reader.
  std::span<const ProjectableField> projectableFields() {
    return space->projectableFields();
  }

  // The representations of the physical catalog that this reader's schema
  // can retrieve: logical roots whose primary source is present and variants
  // with a column, excluding vectors, geo and engine names. Built once for
  // this reader's schema and sorted by name for wildcard prefix traversal.
  // Names and types remain valid while the caller holds the reader.
  std::span<const RetrievableField> retrievableFields();

  // Search selectors for this reader's schema: concrete declarations and
  // dynamic roots observed in its metadata, including their variants.
  // Only indexed term-match targets are included. Sorted by name; built once
  // on first use, independent of column availability.
  std::span<const std::string> searchableFields();

  // Materializes every segment of `mask` (every segment when unrestricted)
  // that is not yet, in parallel: the one batched point where a search opens
  // the segments its root masks kept. Materializing a segment opens its
  // bundle, which every space's view shares.
  void materialize(const SegmentMask& mask = {});
  // Adds the reads materialize(mask) would make, for a fetch ahead of it, so
  // that a search can wait for them without holding a thread
  // (FetchCountdown) and then materialize on resident bytes. Reads of
  // memory-backed and mapped files complete inline.
  void declareMaterialize(const SegmentMask& mask, ReadSet& reads) const;
  // Adds the reads that opening the base of each segment of `mask` makes
  // (what a term dictionary seek opens), for a fetch ahead of parsing.
  void declareBases(const SegmentMask& mask, ReadSet& reads) const;

  // Opens a published snapshot, reusing what it shares with previousReader
  // and the cores its publisher holds open (CommitSnapshot::openCores).
  // Files are secured through `dir` by handle; segments materialize lazily
  // unless `materialize`. Materializations count into `metrics`; parts found
  // unreadable are reported to `onUnreadable`.
  IndexReader(Directory& dir, const CommitSnapshot& snapshot, IndexReader* previousReader = nullptr,
              std::shared_ptr<FilterCache> filterCache = nullptr, std::shared_ptr<Metrics> metrics = nullptr,
              bool materialize = false, std::shared_ptr<const UnreadableHook> onUnreadable = nullptr);
  // Direct tools/tests: the newest durable root, or an empty index.
  explicit IndexReader(Directory& dir, IndexReader* previousReader = nullptr,
                       std::shared_ptr<FilterCache> filterCache = nullptr);
};

}
