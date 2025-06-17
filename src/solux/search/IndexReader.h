#pragma once
#include <span>
#include "solux/reader/PostingsReader.h"
#include "solux/util/screaming.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define IREADER_DEBUG LOG_TRACE
// #define IREADER_DEBUG LOG_DEBUG

namespace solux {

/// LiveDocs holds the live document bitmap for a segment
class LiveDocs {
private:
  screaming::FixedBitSet liveBits;                    // The actual bitset (memory mapped)
  std::shared_ptr<InputFile> deleteFile;              // Keep file alive for memory mapping
  int32_t numLiveDocs = 0;

public:
  LiveDocs() : liveBits(nullptr, 0) {}

  LiveDocs(uint64_t* mappedMemory, int32_t maxDocCount, 
           std::shared_ptr<InputFile>&& file, int32_t numLive)
      : liveBits(mappedMemory, maxDocCount),
        deleteFile(std::move(file)), 
        numLiveDocs(numLive) {}

  // Static factory method to create LiveDocs from delete bitmap file.  Do not call this if liveGen is 0 (no
  // deletes for this segment).  A nullptr is returned in the case that there are deletions but we couldn't
  // find the delete file (this is not necessarily an error, the index could have changed already).
  // If missingFileOK is false, missing files will throw exceptions instead of returning nullptr.
  static std::shared_ptr<LiveDocs> create(Directory& dir, uint64_t segId, uint64_t liveGen, int32_t maxDoc, bool missingFileOK = true);
  
  // Get the underlying FixedBitSet for direct access
  const screaming::FixedBitSet& bitset() const {
    return liveBits;
  }

  int32_t numDeletes() const {
    return liveBits.size() - numLiveDocs;
  }
  
  int32_t numLive() const {
    return numLiveDocs;
  }
  
  int32_t maxDoc() const {
    return liveBits.size();
  }
};


/// IndexReader is thread safe
class IndexReader {
public:

  class Segment {
    const std::shared_ptr<PostingsReader> sharedPostingsReader;
    const std::shared_ptr<LiveDocs> sharedLiveDocs;
  public:
    struct SegmentInfo {
      uint64_t seg_id = 1;                    // Unique identifier for the segment
      uint64_t live_gen = 2;               // What deletes version to use for the segment (0 if no deletes)
      uint64_t min_version = 3;               // Minimum update version in this segment
      uint64_t max_version = 4;               // Maximum update version in this segment
      int32_t  max_doc = 5;                   // Number of documents in this segment (ignoring deletes)
      int32_t  live_docs = 6;                   // Number of live documents in this segment
    };

    const SegmentInfo segInfo;    // metadata read from the index info file about the segment
    const int64_t base;           // global index (ordinal/rank) of the first document in this segment with respect to the list of segments
    const int32_t ord;                // index of this segment in the list of segments

    Segment(std::shared_ptr<PostingsReader>&& postingsReader, std::shared_ptr<LiveDocs>&& liveDocs, 
              SegmentInfo segInfo, int64_t base, int ord)
            :  sharedPostingsReader(std::move(postingsReader)), sharedLiveDocs(std::move(liveDocs)),
                segInfo(segInfo), base(base), ord(ord) {
    }

    PostingsReader& postingsReader() const noexcept {
      return *sharedPostingsReader;
    }

    // returns null if all docs are live (no deletes)
    const LiveDocs* liveDocs() const noexcept {
      return sharedLiveDocs.get();
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


  // TODO: implement postingsReader sharing by passing in another IndexReader for reference.

  // The time in microseconds when this version of the index was committed.  Guaranteed to be strictly increasing
  // with new versions of the index.
  uint64_t commitTime() const noexcept {
    return commitTimeUs;
  }

  std::span<Segment> segments() noexcept {
    return segs;
  }

  int64_t maxDoc() const noexcept {
    return maxdoc;
  }

  IndexReader(Directory& dir);

private:
  std::vector<Segment> segs;
  int64_t maxdoc = 0;
  uint64_t commitTimeUs = 0;
};

}