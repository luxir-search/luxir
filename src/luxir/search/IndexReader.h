// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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
#include "luxir/reader/PostingsReader.h"
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

public:
  LiveDocs() : docSet(FixedBitSet(nullptr, 0)) {}

  LiveDocs(uint64_t* mappedMemory, int32_t maxDocCount, 
           std::shared_ptr<InputFile>&& file, int32_t numLive)
      : docSet(FixedBitSet(mappedMemory, maxDocCount), numLive),
        deleteFile(std::move(file)) {}

  // Static factory method to create LiveDocs from delete bitmap file.  Do not call this if liveGen is 0 (no
  // deletes for this segment).  A nullptr is returned in the case that there are deletions but we couldn't
  // find the delete file (this is not necessarily an error, the index could have changed already).
  // If missingFileOK is false, missing files will throw exceptions instead of returning nullptr.
  static std::shared_ptr<LiveDocs> create(Directory& dir, uint64_t segId, uint64_t liveGen, int32_t maxDoc, bool missingFileOK = true, bool expectSynced = false);
  
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
// open handles when the snapshot opened, and the reader built from them on
// first need. Shared by every snapshot (and the writer) holding the same
// files, opened or not; once built the handles close, since the mappings
// keep the bytes. A build that fails on an I/O or resource error (an
// ApiError such as a failed mmap, std::system_error, std::bad_alloc) throws
// and a later call retries. One that fails on the bytes themselves (a format
// or checksum error, EIO, or a held file absent at decode) marks the part
// unreadable: it is logged at error
// level once, its handles close, every later call rethrows the same error
// without rebuilding, and the part's files are reported to its owner's
// UnreadableHook as suspects, so they are recorded and repaired the way a
// scrub mismatch is.
//
// The hook runs on the thread that found the damage, possibly a writer's
// under its locks: it must not throw, block, or fail the collection inline.
using UnreadableHook = std::function<void(std::vector<std::string> files, std::string reason)>;

template <class Value>
class SegmentPart {
protected:
  SharedLazyCell<Value, false> cell;
  std::unique_ptr<HeldFiles> files;
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

public:
  // The built value, or null when it is not built yet; never builds.
  std::shared_ptr<Value> peek() const noexcept { return cell.peek(); }
  // Whether a build found the part's bytes unreadable; it never retries.
  bool isUnreadable() const noexcept { return unreadable.load(std::memory_order_acquire); }
  // peek() without a reference count; valid while the part lives.
  Value* peekRaw() const noexcept { return cell.peekRaw(); }
};

// A segment's base data: its PostingsReader over the segment files.
class SegmentBase : public SegmentPart<PostingsReader> {
public:
  const uint64_t segId;
  const int32_t maxDoc;
  SegmentBase(uint64_t segId, int32_t maxDoc, std::unique_ptr<HeldFiles> files, std::shared_ptr<Metrics> metrics,
              std::shared_ptr<const UnreadableHook> onUnreadable)
      : SegmentPart(std::move(files), std::move(metrics), std::move(onUnreadable)), segId(segId), maxDoc(maxDoc) {}
  SegmentBase(uint64_t segId, int32_t maxDoc, std::shared_ptr<PostingsReader> opened)
      : SegmentPart(std::move(opened)), segId(segId), maxDoc(maxDoc) {}
  std::shared_ptr<PostingsReader> get();
};

// The live documents of one delete generation.
class SegmentLive : public SegmentPart<LiveDocs> {
public:
  const uint64_t segId;
  const uint64_t liveGen;
  const int32_t maxDoc;
  const int32_t numLive;
  SegmentLive(uint64_t segId, uint64_t liveGen, int32_t maxDoc, int32_t numLive,
              std::unique_ptr<HeldFiles> files, std::shared_ptr<Metrics> metrics,
              std::shared_ptr<const UnreadableHook> onUnreadable)
      : SegmentPart(std::move(files), std::move(metrics), std::move(onUnreadable)), segId(segId), liveGen(liveGen),
        maxDoc(maxDoc),
        numLive(numLive) {}
  std::shared_ptr<LiveDocs> get();
};

// A segment overlay of a known kind (a vector index, the test overlay).
class SegmentOverlay : public SegmentPart<AuxReader> {
public:
  const AuxInfo info;
  SegmentOverlay(AuxInfo info, std::unique_ptr<HeldFiles> files, std::shared_ptr<Metrics> metrics,
                 std::shared_ptr<const UnreadableHook> onUnreadable)
      : SegmentPart(std::move(files), std::move(metrics), std::move(onUnreadable)), info(std::move(info)) {}
  std::shared_ptr<AuxReader> get();
};

  // A snapshot's view of one segment. Opening the snapshot reads only the
  // manifest and the segment's summary and secures its files by handle;
  // the base data, live docs and overlays materialize later, together at the
  // search's batched point (IndexReader::materialize) or on first use.
  class Segment {
    const std::shared_ptr<SegmentBase> sharedBase;
    const std::shared_ptr<SegmentLive> sharedLive;
    const std::vector<std::shared_ptr<SegmentOverlay>> sharedOverlays;
    const std::shared_ptr<const SegmentSummary> sharedSummary;
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

    Segment(std::shared_ptr<SegmentBase> data, std::shared_ptr<SegmentLive> live,
            std::vector<std::shared_ptr<SegmentOverlay>>&& overlays,
            std::shared_ptr<const SegmentSummary>&& summary,
            SegmentInfo segInfo, int64_t docBase, int ord)
            :  sharedBase(std::move(data)), sharedLive(std::move(live)),
                sharedOverlays(std::move(overlays)), sharedSummary(std::move(summary)),
                segInfo(segInfo), base(docBase), ord(ord) {
    }

    // The segment's summary sidecar, loaded when the snapshot opened; null
    // when the segment has none readable, which proves nothing about it.
    const SegmentSummary* summary() const noexcept { return sharedSummary.get(); }

    // Materializes the base data on first use.
    PostingsReader& postingsReader() const {
      if (auto* reader = sharedBase->peekRaw()) return *reader;
      return *sharedBase->get();
    }

    // shared form of postingsReader(): every snapshot holding this physical
    // segment, and the writer, use the one instance
    std::shared_ptr<PostingsReader> postingsReaderShared() const {
      return sharedBase->get();
    }

    // The base data when materialized, else null; never opens.
    PostingsReader* postingsReaderIfOpen() const noexcept { return sharedBase->peekRaw(); }
    bool isOpen() const noexcept { return sharedBase->peekRaw() != nullptr; }
    // Whether the base, live docs and every overlay are materialized.
    bool isMaterialized() const noexcept;
    // Materializes the base, live docs and every overlay.
    void materialize() const;

    // Whether the segment carries a live-docs generation (live_gen > 0), so
    // liveDocs() is non-null and must be consulted. It does not guarantee a
    // deleted document exists (compare numLive() with maxDoc() for that).
    // Never opens the segment's files.
    bool hasDeletes() const noexcept { return sharedLive != nullptr; }

    // returns null if all docs are live (no deletes); materializes them on first use
    LiveDocs* liveDocs() const {
      if (!sharedLive) return nullptr;
      if (auto* live = sharedLive->peekRaw()) return live;
      return sharedLive->get().get();
    }

    // shared form of liveDocs(), for callers that cache views into its mapped bits
    std::shared_ptr<LiveDocs> liveDocsShared() const {
      return sharedLive ? sharedLive->get() : nullptr;
    }

    // Every overlay of a known kind, materialized.
    std::vector<std::shared_ptr<AuxReader>> auxReaders() const;

    // The overlay `name`, materialized on first use; null when absent.
    std::shared_ptr<AuxReader> getAuxReader(std::string_view name) const;

    int32_t maxDoc() const noexcept {
      return segInfo.max_doc;
    }

    // Get number of deleted documents (from the manifest; never opens)
    int32_t numDeletes() const noexcept {
      return segInfo.max_doc - numLive();
    }

    // Get number of live documents (from the manifest; never opens)
    int32_t numLive() const noexcept {
      return sharedLive ? sharedLive->numLive : segInfo.max_doc;
    }
  };





/// IndexReader is thread safe
class IndexReader {
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

  struct HeaderTotals {
    Similarity::FieldStats stats;
    FieldType::flag_type flags = 0;
    FieldType::Type type = FieldType::NONE;
    uint32_t typeFlags = 0;
  };

  // Positive names, header totals and OrdMaps shared by segment sets with the same
  // core generation. The record owns its name and lazily caches each ordinal;
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

  // A physical snapshot is shared directly by readers with different schemas.
  // Its lazy catalogs and maps never retain a reader or schema.
  struct PhysicalCore {
    std::vector<Segment> segs;
    OnceFlag fieldCatalogsOnce;
    std::vector<std::string_view> physicalNames;
    std::vector<ProjectableField> projectable;
    std::vector<std::shared_ptr<AuxReader>> auxReadersList;
    std::shared_ptr<FieldRecords> fields;
    uint64_t coreGeneration = 0;
    int64_t totalMaxDoc = 0;
    int64_t livedocs = 0;
    // A segment's physical identity, which decides what a reopen reuses: the
    // base files its PostingsReader maps, the live-docs file of this
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

    // The segment `segId`, or null.
    const Segment* segment(uint64_t segId) const;

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

  struct Snapshot {
    std::shared_ptr<PhysicalCore> core;
    std::shared_ptr<Schema> schema;
    uint64_t commitTime = 0;
    uint64_t indexGen = 0;
  };
  const uint64_t snapshotTime;
  const uint64_t snapshotGen;
  const std::shared_ptr<PhysicalCore> core;
  const std::shared_ptr<Schema> sharedSchema;
  const std::shared_ptr<FilterCache> sharedFilterCache;
  OnceFlag retrievableOnce;
  std::vector<RetrievableField> retrievable;
  OnceFlag searchableOnce;
  std::vector<std::string> searchable;

  // Opens the snapshot `manifest` (the newest durable root when null),
  // securing every file it needs by an open handle inside the retry loop: a
  // file a newer root retired before its handle opened retries against that
  // root, and once held no deletion can take it. Nothing else is read but the
  // manifest and the segment summaries. Each segment's base comes from
  // `previous` when its files are identical (opened or not), unless
  // `published` (the publisher's open readers, by manifest segment order)
  // hands over a different open one; live docs and overlays likewise come
  // from `previous` when their files are identical. `materialize` opens every
  // segment before returning.
  static Snapshot openSnapshot(Directory& dir, const IndexReader* previous,
                               std::shared_ptr<Schema> schema,
                               std::shared_ptr<const std::vector<std::byte>> manifest,
                               std::span<const std::weak_ptr<PostingsReader>> published,
                               const std::shared_ptr<Metrics>& metrics, bool materialize,
                               const std::shared_ptr<const UnreadableHook>& onUnreadable);

  IndexReader(Snapshot snapshot,
              std::shared_ptr<FilterCache> filterCache);

public:
  const std::shared_ptr<Schema>& schema() const noexcept {
    assert(sharedSchema);
    return sharedSchema;
  }

  // The time in microseconds when this version of the index was committed.  Guaranteed to be strictly increasing
  // with new versions of the index.
  uint64_t commitTime() const noexcept {
    return snapshotTime;
  }

  uint64_t commitId() const noexcept { return snapshotGen; }
  std::string_view incarnation() const noexcept { return core->incarnation; }

  std::span<Segment> segments() noexcept {
    return core->segs;
  }

  // Aux readers for entries in the parsed IndexInfo.aux_indexes (in the same
  // order).  Unknown-kind entries are omitted, so this list may be shorter
  // than IndexInfo.aux_indexes.
  std::span<const std::shared_ptr<AuxReader>> auxReaders() const noexcept {
    return core->auxReadersList;
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
    for (const auto& r : core->auxReadersList) {
      if (r->getName() == name) return r;
    }
    return nullptr;
  }

  int64_t maxDoc() const noexcept {
    return core->totalMaxDoc;
  }

  int64_t liveDocs() const noexcept {
    return core->livedocs;
  }
  
  // Get the core generation for this index reader
  // This can be used as a cache key for structures that depend on segments but don't care about deletes.
  uint64_t coreGen() const noexcept {
    return core->coreGeneration;
  }

  // Validate this schema's type against the field's persisted segment types
  // on access. Unused fields never prevent opening or using a reader.
  FieldSegments field(std::string_view name) const;

  std::shared_ptr<OrdMap> getOrdMap(std::string_view name);

  // This reader's segment `segId`, or null.
  const Segment* findSegment(uint64_t segId) const { return core->segment(segId); }

  FilterCache* filterCache() const noexcept {
    return sharedFilterCache.get();
  }

  // Fields some segment of this reader can project into a
  // DocList: column-backed fields (COLUMN_STORED, any non-BIN type) and the
  // fields held by each stored-fields resource.  Physical and
  // schema-independent - read from segment metadata, so dynamic
  // (suffix-template) fields appear under their concrete names.  Sorted and
  // unique by name, with column presence and the resources storing that name
  // retained for schema-specific source selection. Built once on first use,
  // which materializes the base of every segment (stored resources list their
  // fields only in segment data), and immutable afterwards (segments never
  // change under a reader).  The views point into segment metadata that this
  // reader's PostingsReaders keep mapped, so they are valid for as long as the
  // caller holds the reader.
  std::span<const ProjectableField> projectableFields() {
    return core->projectableFields();
  }

  // The representations of the physical catalog that this reader's schema
  // can retrieve: logical roots whose primary source is present and variants
  // with a column, excluding vectors, geo and engine names. Built once for
  // this reader's schema and sorted by name for wildcard prefix traversal.
  // Names and types remain valid while the caller holds the reader.
  std::span<const RetrievableField> retrievableFields();

  // Search selectors for this reader's schema: concrete declarations and
  // dynamic roots observed in its metadata, including their variants.
  // Only indexed TEXT/STRING/ID search targets are included. Sorted by name;
  // built once on first use, independent of storage or column availability.
  std::span<const std::string> searchableFields();

  // Materializes every segment of `mask` (every segment when unrestricted)
  // that is not yet, in parallel: the one batched point where a search opens
  // the segments its root masks kept. Counts into the reader's metrics.
  void materialize(const SegmentMask& mask = {});

  // Opens a published snapshot, reusing what it shares with previousReader
  // and the readers its publisher holds open (CommitSnapshot::openReaders).
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
