// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cassert>
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
#include "LiveDocsMemory.h"
#include "Similarity.h"
#include "luxir/index/AuxInfo.h"
#include "luxir/reader/AuxReader.h"
#include "luxir/reader/SegmentCore.h"
#include "luxir/reader/RankLiveBitmap.h"
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
  std::shared_ptr<InputFile> deleteFile;
  HeldRange deleteHold;
  std::unique_ptr<uint64_t[]> ownedWords;
  BitDocSet docSet;
  // Charged with heapBytes() while this LiveDocs owns its bitmap; null when mapped.
  LiveDocsMemory::Counter* memory = nullptr;
  mutable std::mutex rankLiveMutex;
  mutable std::vector<std::pair<std::string, std::unique_ptr<const RankLiveBitmap>>> rankLiveViews;

public:
  LiveDocs() : docSet(FixedBitSet(nullptr, 0)) {}

  LiveDocs(uint64_t* bits, int32_t maxDocCount, std::shared_ptr<InputFile> file,
           HeldRange hold, int32_t numLive)
      : deleteFile(std::move(file)), deleteHold(std::move(hold)),
        docSet(FixedBitSet(bits, maxDocCount), numLive) {}

  // A heap bitmap, charged to `counter` until destroyed: a child view derived from
  // root deletes, or a root generation expanded from a SPARSE live docs file.
  LiveDocs(std::unique_ptr<uint64_t[]> words, int32_t maxDoc, int32_t numLive, LiveDocsMemory::Counter& counter);
  ~LiveDocs();

  // Decodes the live docs file `name` of `segment`, `size` bytes long. Bytes
  // that fail validation, or a live count other than `numLive` when it is not
  // negative, throw SegmentCorruptError.
  static std::shared_ptr<LiveDocs> decode(std::shared_ptr<InputFile> file, uint64_t size, std::string_view name,
                                          std::string_view segment, int32_t maxDoc, int32_t numLive);
  // Opens and decodes generation `liveGen` at once, for the writer. Null when
  // the file is missing and missingFileOK is set.
  static std::shared_ptr<LiveDocs> create(Directory& dir, uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen,
                                          uint64_t liveEpoch, int32_t maxDoc, bool missingFileOK = true,
                                          bool expectSynced = false);

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

  // Bytes this LiveDocs holds on the heap; 0 for a mapped DENSE file.
  size_t heapBytes() const {
    return ownedWords ? sizeof(*this) + FixedBitSet::sizeInWords(size()) * sizeof(uint64_t) : 0;
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

// A root live-docs generation. The file is opened with the snapshot, which
// keeps its bytes readable; the bitmap is decoded on first use. The live count
// is the manifest's, so counting needs no read. A decode that fails publishes
// nothing, so the next use retries it.
class SegmentLive {
  std::shared_ptr<InputFile> file;
  std::string name;
  std::string segment;
  uint64_t fileSize;
  int32_t maxDoc;
  int32_t liveCount;
  SharedLazyCell<LiveDocs> value;

public:
  SegmentLive(std::shared_ptr<InputFile> file, std::string name, std::string segment, uint64_t fileSize,
              int32_t maxDoc, int32_t liveCount)
      : file(std::move(file)), name(std::move(name)), segment(std::move(segment)), fileSize(fileSize),
        maxDoc(maxDoc), liveCount(liveCount) {}
  // A bitmap already decoded or built in memory.
  explicit SegmentLive(std::shared_ptr<LiveDocs> decoded)
      : fileSize(0), maxDoc(decoded->size()), liveCount(decoded->numLive()) {
    value.getOrCreate([&] { return std::move(decoded); });
  }

  // Opens the file of generation `liveGen`, whose manifest records `size`
  // bytes (zero when unknown) and `numLive` live rows; a file of another size
  // is corrupt. Null when the file is missing and missingFileOK is set, so
  // the caller can reload the manifest.
  static std::shared_ptr<SegmentLive> open(Directory& dir, uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen,
                                           uint64_t liveEpoch, int32_t maxDoc, int32_t numLive, uint64_t size,
                                           bool missingFileOK, bool expectSynced);

  int32_t numLive() const noexcept { return liveCount; }
  int32_t size() const noexcept { return maxDoc; }
  bool decoded() const noexcept { return value.peek() != nullptr; }
  std::shared_ptr<LiveDocs> shared();
  LiveDocs* get() {
    auto* decodedDocs = value.peek();
    return decodedDocs ? decodedDocs : shared().get();
  }
};

// A segment overlay of known kind. Its files are opened with the snapshot,
// which keeps their bytes readable; its reader is opened on first use, under
// the kind's OverlayPolicy. A kind opens from these files and never from the
// directory, so no kind reads before its first use or after its files are
// retired. A failed open publishes nothing, so the next use retries it,
// except that an advisory overlay with invalid bytes stays absent.
class SegmentOverlay {
  AuxInfo info;
  OverlayPolicy policy;
  std::string segment;
  std::vector<std::shared_ptr<InputFile>> files;
  SharedLazyCell<AuxReader> reader;
  std::atomic<bool> unusable = false;  // An advisory overlay whose bytes were found invalid.

public:
  SegmentOverlay(AuxInfo info, OverlayPolicy policy, std::string segment,
                 std::vector<std::shared_ptr<InputFile>> files)
      : info(std::move(info)), policy(policy), segment(std::move(segment)), files(std::move(files)) {}

  // Whether readers open overlays of this kind; others are left out.
  static bool knownKind(std::string_view kind);
  // Opens the entry's files. Null when one is missing and missingFileOK is
  // set, so the caller can reload the manifest.
  static std::shared_ptr<SegmentOverlay> open(Directory& dir, const luxir::api::AuxIndexInfo& entry,
                                              std::string segment, bool missingFileOK);

  std::string_view name() const noexcept { return info.name; }
  std::string_view kind() const noexcept { return info.kind; }
  uint64_t gen() const noexcept { return info.gen; }
  uint64_t builtCoreGen() const noexcept { return info.built_core_gen; }
  OverlayPolicy openPolicy() const noexcept { return policy; }
  bool isOpen() const noexcept { return reader.peek() != nullptr; }
  bool isUnusable() const noexcept { return unusable.load(std::memory_order_relaxed); }
  // The reader, opened on first use; null for an advisory overlay whose bytes are invalid.
  std::shared_ptr<AuxReader> get();
};

// One lazy value for a path, bundle and root live generation. Shared by leaves
// of successive snapshots; constructing a view does not construct its bitmap.
class ChildLiveDocs {
  std::shared_ptr<SegmentCore> core;
  std::shared_ptr<SegmentLive> root;
  std::string path;
  std::atomic<int32_t> knownCard{-1};
  SharedLazyCell<LiveDocs> value;

public:
  ChildLiveDocs(std::shared_ptr<SegmentCore> core, std::shared_ptr<SegmentLive> root,
                std::string path) : core(std::move(core)), root(std::move(root)), path(std::move(path)) {}
  std::shared_ptr<LiveDocs> get();
  int32_t knownCount() const noexcept { return knownCard.load(std::memory_order_acquire); }
};

  class Segment {
    const std::shared_ptr<SegmentCore> sharedSegmentCore;
    PostingsReader* const spaceReader;
    const int32_t rowCount;
    const int32_t liveCount;
    const std::shared_ptr<SegmentLive> sharedLiveDocs;
    const std::shared_ptr<ChildLiveDocs> childLiveDocs;
    const std::vector<std::shared_ptr<SegmentOverlay>> sharedOverlays;
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

    Segment(std::shared_ptr<SegmentCore>&& segmentCore, std::shared_ptr<SegmentLive>&& liveDocs,
              std::vector<std::shared_ptr<SegmentOverlay>>&& overlays,
              SegmentInfo segInfo, int64_t base, int ord, PostingsReader* spaceReader,
              std::shared_ptr<ChildLiveDocs> childLiveDocs = {})
            :  sharedSegmentCore(std::move(segmentCore)), spaceReader(spaceReader),
                rowCount(spaceReader ? spaceReader->maxDoc() : 0),
                liveCount(liveDocs ? liveDocs->numLive() : rowCount),
                sharedLiveDocs(std::move(liveDocs)),
                childLiveDocs(std::move(childLiveDocs)),
                sharedOverlays(std::move(overlays)),
                segInfo(segInfo), base(base), ord(ord) {
      assert(!sharedLiveDocs || sharedLiveDocs->size() == rowCount);
    }

    PostingsReader& postingsReader() const noexcept {
      assert(spaceReader);
      return *spaceReader;
    }

    bool hasSpace() const noexcept { return spaceReader != nullptr; }

    SpaceOrdinal spaceOrdinal() const noexcept {
      return spaceReader ? spaceReader->spaceOrdinal() : (SpaceOrdinal)UINT32_MAX;
    }

    SegmentCore& segmentCore() const noexcept { return *sharedSegmentCore; }

    // Pins the immutable bundle and all of its space readers.
    std::shared_ptr<SegmentCore> segmentCoreShared() const noexcept {
      return sharedSegmentCore;
    }

    // Whether this space has deleted rows. Root writers create a live-docs
    // generation only with at least one deletion. Never opens files.
    bool hasDeletes() const { return numLive() != rowCount; }

    // Metadata probes must not construct a derived live view. Until it has
    // been used, a child count is an interval rather than an exact count.
    std::pair<int32_t, int32_t> liveCountBounds() const noexcept {
      int32_t count = childLiveDocs ? childLiveDocs->knownCount() : liveCount;
      return count < 0 ? std::pair{0, rowCount} : std::pair{count, count};
    }

    bool mayHaveDeletes() const noexcept { return liveCountBounds().first != rowCount; }

    // Null if all docs are live (no deletes). Decodes the bitmap on first use.
    LiveDocs* liveDocs() const {
      if (childLiveDocs) return childLiveDocs->get().get();
      return sharedLiveDocs ? sharedLiveDocs->get() : nullptr;
    }

    // The segment's live docset, or null when there are no deletes.
    BitDocSet* liveDocSet() const {
      auto* live = liveDocs();
      return live ? &live->docset() : nullptr;
    }

    // shared form of liveDocs(), for callers that cache views into its mapped bits
    std::shared_ptr<LiveDocs> liveDocsShared() const {
      if (childLiveDocs) return childLiveDocs->get();
      return sharedLiveDocs ? sharedLiveDocs->shared() : nullptr;
    }

    // The segment's overlays of known kinds, opened or not.
    std::span<const std::shared_ptr<SegmentOverlay>> overlays() const noexcept {
      return sharedOverlays;
    }

    // The overlay `name`, opened on first use. Null when the segment has none,
    // or has an advisory one whose bytes are invalid. Throws when its open
    // fails otherwise.
    std::shared_ptr<AuxReader> getAuxReader(std::string_view name) const {
      for (const auto& overlay : sharedOverlays) {
        if (overlay->name() == name) return overlay->get();
      }
      return nullptr;
    }

    int32_t maxDoc() const noexcept {
      return rowCount;
    }

    // Number of deleted rows in this space.
    int32_t numDeletes() const {
      return maxDoc() - numLive();
    }

    // Number of live rows in this space.
    int32_t numLive() const {
      if (!childLiveDocs) return liveCount;
      auto* live = liveDocs();
      return live ? live->numLive() : rowCount;
    }
  };





/// Thread-safe scoped view of one space in an immutable snapshot.
class IndexReader : public std::enable_shared_from_this<IndexReader> {
  struct FieldNameEntry;
  struct FieldTotals;

public:
  using Segment = ::luxir::Segment;

  // A borrowed view, valid while its reader lives. Null means absent.
  // at() lazily resolves ordinals and field records in this core's segments.
  // stats() and segFlags() resolve every segment for field totals,
  // including deleted docs.
  class FieldSegments {
    friend class IndexReader;
    const FieldNameEntry* entry = nullptr;
    const Segment* segments = nullptr;
    FieldSegments(const FieldNameEntry* entry, const Segment* segments)
        : entry(entry), segments(segments) {}
    int32_t ordinalAt(size_t segment) const;
    const FieldTotals& totals() const;
    static FieldRecordHeader mergeHeaders(std::span<const std::optional<FieldRecordHeader>> headers);

  public:
    FieldSegments() = default;
    const DecodedFieldRecord* at(size_t segment) const;
    const DecodedFieldRecord* at(const Segment& segment) const {
      assert(!entry || (segment.ord >= 0 && (size_t)segment.ord < size()
                         && &segments[segment.ord] == &segment));
      return at((size_t)segment.ord);
    }
    std::string_view name() const noexcept;
    bool exists() const noexcept;
    size_t size() const noexcept;
    const Similarity::FieldStats& stats() const;
    // Union of the segment-specific flags over the field's segments.
    uint8_t segFlags() const;
    // The field's record header in `segment`, or null when the segment has
    // no record for it. Memoized with the field totals; never decodes a part.
    const FieldRecordHeader* header(size_t segment) const;
    // The headers of every segment with a record, merged: summed counts and
    // the union of their bounds (none when some segment with values has
    // none). Describes the segments as written, deletes included.
    const FieldRecordHeader& mergedHeader() const;
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

  struct FieldTotals {
    Similarity::FieldStats stats;
    uint8_t segFlags = 0;  // See FieldSegmentFlags.
    FieldType::Type type = FieldType::NONE;
    uint32_t typeParameters = 0;
    // Each segment's record header, empty where it has no record. Names and
    // dictionary bounds borrow the segments' mapped catalogs.
    std::vector<std::optional<FieldRecordHeader>> headers;
    FieldRecordHeader merged;  // See FieldSegments::mergedHeader().

    size_t bytes() const noexcept { return sizeof(*this) + headers.capacity() * sizeof(headers[0]); }
  };

  // Positive names, field totals and OrdMaps shared by segment sets on one path
  // with the same core generation. The entry owns its name and lazily caches each ordinal;
  // it holds no Segment or reader, so views can use different open readers.
  struct FieldNameEntry {
    static constexpr int32_t UNRESOLVED = -2;
    std::string name;
    size_t hash;
    size_t numSegments;
    std::unique_ptr<std::atomic<int32_t>[]> ordinals;
    mutable std::atomic<FieldTotals*> totals{nullptr};
    mutable SharedLazyCell<OrdMap, true> ordMap;
    size_t bytes;

    FieldNameEntry(std::string_view name, size_t hash, size_t numSegments, size_t first, int32_t ordinal);
    ~FieldNameEntry();
  };

  // Entries by name. Insert-only, so a lookup takes no lock and writes no
  // shared memory. Inserts serialize; one that grows the table publishes a
  // doubled copy, and replaced tables live as long as the entries because a
  // lookup may still be probing one.
  class FieldCache {
    struct Table {
      size_t mask;
      std::unique_ptr<std::atomic<const FieldNameEntry*>[]> slots;
      explicit Table(size_t capacity);
      const FieldNameEntry* find(std::string_view name, size_t hash) const noexcept;
      void place(const FieldNameEntry* entry) noexcept;
    };
    std::atomic<const Table*> current{nullptr};
    std::mutex insertMutex;
    std::vector<std::unique_ptr<Table>> tables;
    std::vector<std::unique_ptr<FieldNameEntry>> entries;

  public:
    static constexpr size_t BYTES_PER_FIELD =
        sizeof(std::unique_ptr<FieldNameEntry>) + sizeof(std::atomic<const FieldNameEntry*>);

    const FieldNameEntry* find(std::string_view name, size_t hash) const noexcept {
      const Table* table = current.load(std::memory_order_acquire);
      return table ? table->find(name, hash) : nullptr;
    }
    // Returns the entry kept for its name: this one, or one inserted first.
    const FieldNameEntry* insert(std::unique_ptr<FieldNameEntry> entry);
  };

  // Per-space physical state can be shared by schema-only publications.
  // Its lazy field name lists and maps never retain a reader or schema.
  struct SpaceState {
    std::string path;
    std::vector<Segment> segs;
    OnceFlag fieldNamesOnce;
    std::vector<std::string_view> physicalNames;
    std::vector<ProjectableField> projectable;
    std::shared_ptr<FieldCache> fields;
    int64_t totalMaxDoc = 0;
    int64_t livedocs = 0;
    OnceFlag liveOnce;

    void buildFieldNameLists();
    std::span<const ProjectableField> projectableFields() {
      buildFieldNameLists();
      return projectable;
    }
    std::span<const std::string_view> physicalFieldNames() {
      buildFieldNameLists();
      return physicalNames;
    }
  };

  struct PhysicalCore {
    struct ChildLives {
      std::mutex mutex;
      std::map<std::string, std::shared_ptr<ChildLiveDocs>, std::less<>> paths;
    };
    struct Bundle {
      std::shared_ptr<SegmentCore> core;
      std::shared_ptr<SegmentLive> live;
      std::vector<std::shared_ptr<SegmentOverlay>> aux;
      Segment::SegmentInfo info;
      int32_t ord;
      std::shared_ptr<ChildLives> childLives;
    };
    std::vector<Bundle> bundles;
    std::vector<std::shared_ptr<AuxReader>> auxReadersList;
    uint64_t coreGeneration = 0;
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
      CatalogRange catalog;
      int32_t maxDoc;
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

    Snapshot(OpenedSnapshot opened, std::shared_ptr<FilterCache> filterCache)
        : core(std::move(opened.core)), schema(std::move(opened.schema)),
          filterCache(std::move(filterCache)), commitTime(opened.commitTime), indexGen(opened.indexGen) {}
  };
  const std::shared_ptr<Snapshot> snapshot;
  const std::shared_ptr<SpaceState> space;
  OnceFlag retrievableOnce;
  std::vector<RetrievableField> retrievable;
  OnceFlag searchableOnce;
  std::vector<std::string> searchable;

  // Opens the snapshot `manifest` (the newest durable root when null). Each
  // segment's core comes from `published` (the publisher's open
  // cores, by manifest segment order), else from `previous` when its files
  // are identical, else is opened; its live docs come from `previous` when
  // their file is identical.
  static OpenedSnapshot openSnapshot(Directory& dir, const IndexReader* previous,
                               std::shared_ptr<Schema> schema,
                               std::shared_ptr<const std::vector<std::byte>> manifest,
                               std::span<const std::weak_ptr<SegmentCore>> published = {});

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
  // retained for schema-specific source selection. Built once on first use
  // and immutable afterwards (segments never
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

  // Opens a published snapshot, reusing what it shares with previousReader
  // and the cores its publisher holds open (CommitSnapshot::openCores).
  IndexReader(Directory& dir, const CommitSnapshot& snapshot, IndexReader* previousReader = nullptr,
              std::shared_ptr<FilterCache> filterCache = nullptr);
  // Direct tools/tests: the newest durable root, or an empty index.
  explicit IndexReader(Directory& dir, IndexReader* previousReader = nullptr,
                       std::shared_ptr<FilterCache> filterCache = nullptr);
};

}
