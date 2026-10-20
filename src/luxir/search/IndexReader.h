// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "DocSet.h"
#include "Similarity.h"
#include "luxir/reader/AuxReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/SourceReader.h"
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


  class Segment {
    const std::shared_ptr<PostingsReader> sharedPostingsReader;
    const std::shared_ptr<LiveDocs> sharedLiveDocs;
    const std::vector<std::shared_ptr<AuxReader>> sharedAuxReaders;
    const std::shared_ptr<SourceReader> sharedSource;
  public:
    friend class IndexReader;

    struct SegmentInfo {
      uint64_t seg_id;          // Unique identifier for the segment
      uint64_t live_gen;        // What deletes version to use for the segment (0 if no deletes)
      uint64_t source_gen = 0;  // Generation of the segment's source map (0 if its docs keep no source)
      uint64_t min_version;     // Minimum update version in this segment
      uint64_t max_version;     // Maximum update version in this segment
      uint64_t commit_time;     // first time this segment was committed as part of the index
      int32_t  max_doc;         // Number of live documents in this segment
      int32_t  live_docs;       // Number of live documents in this segment
    };

    const SegmentInfo segInfo;    // metadata read from the index info file about the segment
    const int64_t base;           // global index (ordinal/rank) of the first document in this segment with respect to the list of segments
    const int32_t ord;            // index of this segment in the list of segments

    Segment(std::shared_ptr<PostingsReader>&& postingsReader, std::shared_ptr<LiveDocs>&& liveDocs,
              std::vector<std::shared_ptr<AuxReader>>&& auxReaders,
              SegmentInfo segInfo, int64_t base, int ord,
              std::shared_ptr<SourceReader> source = {})
            :  sharedPostingsReader(std::move(postingsReader)), sharedLiveDocs(std::move(liveDocs)),
                sharedAuxReaders(std::move(auxReaders)), sharedSource(std::move(source)),
                segInfo(segInfo), base(base), ord(ord) {
    }

    // The segment's stored documents, or null if none of its docs keep a source.
    SourceReader* source() const noexcept {
      return sharedSource.get();
    }
    std::shared_ptr<SourceReader> sourceShared() const noexcept {
      return sharedSource;
    }

    PostingsReader& postingsReader() const noexcept {
      return *sharedPostingsReader;
    }

    // shared form of postingsReader(): every snapshot holding this physical
    // segment, and the writer, use the one instance
    std::shared_ptr<PostingsReader> postingsReaderShared() const noexcept {
      return sharedPostingsReader;
    }

    // returns null if all docs are live (no deletes)
    LiveDocs* liveDocs() const noexcept {
      return sharedLiveDocs.get();
    }

    // shared form of liveDocs(), for callers that cache views into its mapped bits
    std::shared_ptr<LiveDocs> liveDocsShared() const noexcept {
      return sharedLiveDocs;
    }

    std::span<const std::shared_ptr<AuxReader>> auxReaders() const noexcept {
      return sharedAuxReaders;
    }

    std::shared_ptr<AuxReader> getAuxReader(std::string_view name) const {
      for (const auto& r : sharedAuxReaders) {
        if (r->getName() == name) return r;
      }
      return nullptr;
    }

    int32_t maxDoc() const noexcept {
      return segInfo.max_doc;
    }

    // Get number of deleted documents
    int32_t numDeletes() const noexcept {
      return sharedLiveDocs ? sharedLiveDocs->numDeletes() : 0;
    }

    // Get number of live documents
    int32_t numLive() const noexcept {
      return sharedLiveDocs ? sharedLiveDocs->numLive() : segInfo.max_doc;
    }
  };





/// IndexReader is thread safe
class IndexReader {
  struct FieldRecord;
  struct HeaderTotals;

public:
  using Segment = ::luxir::Segment;

  // A borrowed view, valid while its reader lives. Null means absent.
  // at() lazily resolves ordinals and headers in this core's segments.
  // stats() and flagsUnion() resolve every segment for header totals,
  // including deleted docs.
  class FieldSegments {
    friend class IndexReader;
    const FieldRecord* record = nullptr;
    const Segment* segments = nullptr;
    FieldSegments(const FieldRecord* record, const Segment* segments)
        : record(record), segments(segments) {}
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

    FieldRecord(std::string_view name, size_t hash, size_t numSegments, size_t first, int32_t ordinal);
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
    OnceFlag projectableOnce;
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

    std::span<const ProjectableField> projectableFields();
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

  // Opens the snapshot `manifest` (the newest durable root when null). Each
  // segment's PostingsReader comes from `published` (the publisher's open
  // readers, by manifest segment order), else from `previous` when its files
  // are identical, else is opened; its live docs come from `previous` when
  // their file is identical.
  static Snapshot openSnapshot(Directory& dir, const IndexReader* previous,
                               std::shared_ptr<Schema> schema,
                               std::shared_ptr<const std::vector<std::byte>> manifest,
                               std::span<const std::weak_ptr<PostingsReader>> published = {});

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
  // retained for schema-specific source selection. Built once on first use
  // and immutable afterwards (segments never
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

  // Opens a published snapshot, reusing what it shares with previousReader
  // and the readers its publisher holds open (CommitSnapshot::openReaders).
  IndexReader(Directory& dir, const CommitSnapshot& snapshot, IndexReader* previousReader = nullptr,
              std::shared_ptr<FilterCache> filterCache = nullptr);
  // Direct tools/tests: the newest durable root, or an empty index.
  explicit IndexReader(Directory& dir, IndexReader* previousReader = nullptr,
                       std::shared_ptr<FilterCache> filterCache = nullptr);
};

}
