#pragma once
#include <memory>
#include <span>
#include <vector>

#include "DocSet.h"
#include "OrdMap.h"
#include "solux/reader/AuxReader.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/BlockBounds.h"
#include "solux/util/screaming.h"
#include "solux/util/SharedLazyMap.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define IREADER_DEBUG LOG_TRACE
// #define IREADER_DEBUG LOG_DEBUG

namespace solux {
class OrdMap;

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
    std::vector<std::pair<std::string, std::shared_ptr<BlockBounds>>> blockBoundsFields;
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

    const BlockBounds* blockBounds(std::string_view field) const {
      for (const auto& entry : blockBoundsFields) {
        if (entry.first == field) return entry.second.get();
      }
      return nullptr;
    }

  private:
    void attachBlockBounds(std::string field, std::shared_ptr<BlockBounds> bounds) {
      blockBoundsFields.emplace_back(std::move(field), std::move(bounds));
    }

  public:

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
  // TODO: implement postingsReader sharing by passing in another IndexReader for reference.

  using Segment = ::solux::Segment;

  // The time in microseconds when this version of the index was committed.  Guaranteed to be strictly increasing
  // with new versions of the index.
  uint64_t commitTime() const noexcept {
    return commitTimeUs;
  }

  std::span<Segment> segments() noexcept {
    return segs;
  }

  // Aux readers for entries in the parsed IndexInfo.aux_indexes (in the same
  // order).  Unknown-kind entries are omitted, so this list may be shorter
  // than IndexInfo.aux_indexes.
  std::span<const std::shared_ptr<AuxReader>> auxReaders() const noexcept {
    return auxReadersList;
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
    for (const auto& r : auxReadersList) {
      if (r->getName() == name) return r;
    }
    return nullptr;
  }

  int64_t maxDoc() const noexcept {
    return totalMaxDoc;
  }

  int64_t liveDocs() const noexcept {
    return livedocs;
  }
  
  // Get the core generation for this index reader
  // This can be used as a cache key for structures that depend on segments but don't care about deletes.
  uint64_t coreGen() const noexcept {
    return coreGeneration;
  }

  std::shared_ptr<OrdMap> getOrdMap(std::string_view field) {
    return ordMaps->getOrCreate(std::string(field), [this, field]() {
      return OrdMap::build(field, *this);
    });
  }
  
  // Get the number of cached OrdMaps (for testing)
  size_t getOrdMapCacheSize() const {
    return ordMaps->dataMap.size();
  }

  IndexReader(Directory& dir, IndexReader* previousReader = nullptr);

private:
  std::vector<Segment> segs;
  std::vector<std::shared_ptr<AuxReader>> auxReadersList;
  uint64_t coreGeneration = 0;
  int64_t totalMaxDoc = 0;
  int64_t livedocs = 0;
  uint64_t commitTimeUs = 0;
  
public:
  // Made public for testing purposes only
  std::shared_ptr<SharedLazyMap<std::string, OrdMap>> ordMaps; // field -> OrdMap
};

}
