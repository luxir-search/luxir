// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "DocSet.h"
#include "OrdMap.h"
#include "luxir/reader/AuxReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/util/screaming.h"
#include "luxir/util/SharedLazyMap.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define IREADER_DEBUG LOG_TRACE
// #define IREADER_DEBUG LOG_DEBUG

namespace luxir {
class FieldType;
class FilterCache;
class OrdMap;
class Schema;

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
  public:
    friend class IndexReader;

    struct SegmentInfo {
      uint64_t seg_id;          // Unique identifier for the segment
      uint64_t live_gen;        // What deletes version to use for the segment (0 if no deletes)
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
              SegmentInfo segInfo, int64_t base, int ord)
            :  sharedPostingsReader(std::move(postingsReader)), sharedLiveDocs(std::move(liveDocs)),
                sharedAuxReaders(std::move(auxReaders)),
                segInfo(segInfo), base(base), ord(ord) {
    }

    PostingsReader& postingsReader() const noexcept {
      return *sharedPostingsReader;
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
public:
  using Segment = ::luxir::Segment;

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

  // A physical snapshot is shared directly by readers with different schemas.
  // Its lazy catalogs and maps never retain a reader or schema.
  struct PhysicalCore {
    std::vector<Segment> segs;
    std::once_flag projectableOnce;
    std::vector<ProjectableField> projectable;
    std::vector<std::shared_ptr<AuxReader>> auxReadersList;
    std::shared_ptr<SharedLazyMap<std::string, OrdMap>> ordMaps;
    uint64_t coreGeneration = 0;
    int64_t totalMaxDoc = 0;
    int64_t livedocs = 0;
    struct SegmentFiles {
      uint64_t segId;
      std::vector<FileDescriptor> files;
      std::vector<std::vector<FileDescriptor>> overlays;
      bool operator==(const SegmentFiles&) const = default;
    };
    std::vector<SegmentFiles> segmentFiles;
    std::vector<std::vector<FileDescriptor>> auxFiles;
    std::string incarnation;

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
  std::once_flag retrievableOnce;
  std::vector<RetrievableField> retrievable;

  static Snapshot openSnapshot(Directory& dir, const IndexReader* previous,
                               std::shared_ptr<Schema> schema,
                               std::shared_ptr<const std::vector<std::byte>> manifest);

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

  std::shared_ptr<OrdMap> getOrdMap(std::string_view field) {
    return core->ordMaps->getOrCreate(std::string(field), [this, field]() {
      return OrdMap::build(field, *this);
    });
  }
  
  // Get the number of cached OrdMaps (for testing)
  size_t getOrdMapCacheSize() const {
    return core->ordMaps->dataMap.size();
  }

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

  // Direct tools/tests may omit the schema if they only use physical state.
  IndexReader(Directory& dir, IndexReader* previousReader = nullptr,
              std::shared_ptr<FilterCache> filterCache = nullptr,
              std::shared_ptr<Schema> schema = nullptr,
              std::shared_ptr<const std::vector<std::byte>> manifest = {});
};

}
