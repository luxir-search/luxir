#pragma once
#include <span>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <cstring>
#include "solux/reader/PostingsReader.h"
#include "solux/util/screaming.h"
#include "protos/solux_types.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

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
  static std::shared_ptr<LiveDocs> create(Directory& dir, uint64_t segId, uint64_t liveGen, int32_t maxDoc, bool missingFileOK = true) {
    assert(liveGen > 0 && maxDoc > 0);
    std::string deleteFileName = Postings::getDeleteFileName(
      Postings::getSortableString(segId), liveGen);
    
    auto deleteFile = dir.openFile(deleteFileName);
    if (deleteFile == nullptr) {
      if (missingFileOK) {
        // Delete file not found - return nullptr instead of throwing
        IREADER_DEBUG("Delete file {} not found for segment {} (liveGen={})", 
                  deleteFileName, segId, liveGen);
        return nullptr;
      } else {
        throw std::filesystem::filesystem_error(
                std::format("Delete file {} not found for segment {} (liveGen={})", 
                           deleteFileName, segId, liveGen),
                std::make_error_code(std::errc::no_such_file_or_directory));
      }
    }
    IREADER_DEBUG("Successfully opened delete file {} for segment {}", deleteFileName, segId);
    
    InputStream deleteStream = deleteFile->getInputStream();
    
    // Validate file format
    const char* data = deleteStream.ptr();
    size_t dataSize = deleteStream.size();
    
    // Define offsets based on field sizes
    auto headerSize =  Postings::SOLUX_HEADER.size() + sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t);
    // Minimum size check
    if (dataSize < headerSize) {
      throw std::runtime_error(std::format("Delete file {} is too small: {} bytes", deleteFileName, dataSize));
    }
    
    // Check magic number
    if (std::memcmp(data, Postings::SOLUX_HEADER.data(), Postings::SOLUX_HEADER.size()) != 0) {
      throw std::runtime_error(std::format("Delete file {} has invalid magic number", deleteFileName));
    }

    deleteStream.skip(Postings::SOLUX_HEADER.size());
    uint64_t format = (uint64_t)deleteStream.readLong();
    int32_t fileMaxDoc = deleteStream.readInt();
    int32_t numLiveDocs = deleteStream.readInt();

    if (format != 1) {
      throw std::runtime_error(std::format("Delete file {} has unsupported format: {}", deleteFileName, format));
    }

    if (fileMaxDoc != maxDoc) {
      throw std::runtime_error(std::format("Delete file {} maxDoc mismatch: {} (expected {})", 
                deleteFileName, fileMaxDoc, maxDoc));
    }
    
    if (numLiveDocs < 0 || numLiveDocs > maxDoc) {
      throw std::runtime_error(std::format("Delete file {} has invalid numLiveDocs: {} (maxDoc={})", 
                deleteFileName, numLiveDocs, maxDoc));
    }
    
    // Validate file size
    int64_t expectedSize = deleteStream.offset() + screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
    if (deleteStream.size() != expectedSize) {
      throw std::runtime_error(std::format("Delete file {} size mismatch: {} bytes (expected {})", 
                deleteFileName, dataSize, expectedSize));
    }
    
    // FixedBitSet with live docs - memory map directly!
    uint64_t* liveBitsMemory = (uint64_t*)deleteStream.ptr();

    IREADER_DEBUG("Loaded {} deleted documents from {} for segment {}, {} live docs", 
              maxDoc - numLiveDocs, deleteFileName, segId, numLiveDocs);
    
    return std::make_shared<LiveDocs>(liveBitsMemory, maxDoc, std::move(deleteFile), numLiveDocs);
  }
  
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


  IndexReader(Directory& dir) {
    // because old segments could be merged away before we have a chance to read them, we need
    // to check if there is a new index info file and retry the open if so.
    // This could be optimized by saving the segments we did read properly in case they are still in the index.
    // But we should really have a postings getter abstraction that can provide already opened readers and livedocs
    uint64_t lastCommitTime = 0;
    bool retry = false;
    do {
      if (retry) {
        IREADER_DEBUG("Retrying IndexReader open");
        segs.clear();
        maxdoc = 0;
        retry = false;
      }
      std::shared_ptr<InputFile> inputFile = dir.openFile(Postings::INDEX_INFO_FILE);
      if (inputFile == nullptr) {
        IREADER_DEBUG("No {} file, Empty IndexReader", Postings::INDEX_INFO_FILE);
      } else {
        IREADER_DEBUG("Opening IndexReader");
        InputStream segmentsIs = inputFile->getInputStream();
        
        // Read the protobuf message
        solux::proto::IndexInfo indexInfo;
        google::protobuf::io::ArrayInputStream arrayStream(segmentsIs.ptr(), segmentsIs.left());
        google::protobuf::io::CodedInputStream codedStream(&arrayStream);
        
        if (!indexInfo.ParseFromCodedStream(&codedStream)) {
          throw std::runtime_error("Failed to parse IndexInfo protobuf");
        }

        commitTimeUs = indexInfo.commit_time();
        bool missingFileOK = true;  // Allow missing files on first attempt
        IREADER_DEBUG("\tOpening IndexReader, commitTime={} nSegs={} gen={}", indexInfo.commit_time(), indexInfo.segments_size(), indexInfo.index_gen());
        if (commitTimeUs == lastCommitTime) {
          // No new commit, continue with missingFileOK=false so we get proper exceptions
          IREADER_DEBUG("Retry index open did not get new IndexInfo file, will try with missingFileOK=false.");
          missingFileOK = false;  // On retry, we expect files to be present
        }
        lastCommitTime = commitTimeUs;

        segs.reserve(indexInfo.segments_size());
        for (int i = 0; i < indexInfo.segments_size(); i++) {
          const auto& segment = indexInfo.segments(i);
          uint64_t segId = segment.seg_id();
          uint64_t liveGen = segment.live_gen();
          int32_t nDocs = segment.max_doc();
          unused(nDocs);

          auto postingsReader = PostingsReader::create(dir, segId, missingFileOK);
          if (!postingsReader) {
            // Failed to create PostingsReader (segment files not found) - trigger retry
            retry = true;
            break;  // Break out of the segments loop to retry
          }
          
          std::shared_ptr<LiveDocs> liveDocs;
          if (liveGen > 0) {
            liveDocs = LiveDocs::create(dir, segId, liveGen, nDocs, missingFileOK);
            if (!liveDocs) {
              // Failed to create LiveDocs (delete file not found) - trigger retry
              retry = true;
              break;  // Break out of the segments loop to retry
            }
          }
          // If liveGen == 0, liveDocs remains nullptr (no deletes)

          Segment::SegmentInfo segmentInfo;
          segmentInfo.seg_id = segId;
          segmentInfo.live_gen = liveGen;
          segmentInfo.min_version = segment.min_version();
          segmentInfo.max_version = segment.max_version();
          segmentInfo.max_doc = nDocs;
          segmentInfo.live_docs = segment.live_docs();

          segs.emplace_back(std::move(postingsReader), std::move(liveDocs), segmentInfo, maxdoc, i);
          maxdoc += segs.back().postingsReader().numDocs();
          assert(nDocs == segs.back().postingsReader().numDocs());
        }
      }
    } while(retry);
    IREADER_DEBUG("IndexReader opened with {} segments and {} docs, commitTime={}", segs.size(), maxdoc, commitTimeUs);
  }

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

private:
  std::vector<Segment> segs;
  int64_t maxdoc = 0;
  uint64_t commitTimeUs = 0;
};

}