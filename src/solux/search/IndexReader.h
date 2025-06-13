#pragma once
#include <span>
#include "solux/reader/PostingsReader.h"
#include "solux/util/screaming.h"
#include "protos/solux_types.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define IREADER_DEBUG LOG_TRACE
// #define IREADER_DEBUG LOG_DEBUG

namespace solux {

/// DeletedDocs holds the delete bitmap for a segment
class DeletedDocs {
private:
  struct DeleteData {
    screaming::BitSet bits;
    std::shared_ptr<InputFile> deleteFile;
    InputStream deleteStream;
    int32_t numDeleted = 0;
  };

  std::unique_ptr<DeleteData> deleteData;

public:
  DeletedDocs() = default;
  
  // Load delete bitmap from file
  DeletedDocs(Directory& dir, uint64_t segId, uint64_t deletesGen) {
    if (deletesGen == 0) {
      return; // No deletes
    }
    
    std::string deleteFileName = Postings::getDeleteFileName(
      Postings::getSortableString(segId), deletesGen);
    
    auto deleteFile = dir.openFile(deleteFileName);
    if (deleteFile == nullptr) {
      LOG_WARN("Delete file {} not found for segment {} (deletesGen={})", deleteFileName, segId, deletesGen);
      return;
    }
    LOG_DEBUG("Successfully opened delete file {} for segment {}", deleteFileName, segId);
    
    InputStream deleteStream = deleteFile->getInputStream();
    screaming::BitSet deleteBitset(deleteStream.ptr() + deleteStream.size());

    int32_t numDeleted = 0;
    // Count deleted documents
    screaming::BitSet::Iterator iter(deleteBitset);
    int32_t deletedDoc = iter.next();
    while (deletedDoc != screaming::BitSet::END) {
      numDeleted++;
      deletedDoc = iter.next();
    }

    deleteData = std::make_unique<DeleteData>(
      std::move(deleteBitset),
      std::move(deleteFile),
      std::move(deleteStream),
      numDeleted
    );

    LOG_DEBUG("Loaded {} deleted documents from {} for segment {}", 
              numDeleted, deleteFileName, segId);
  }
  
  bool isDeleted(int32_t docId) const {
    if (!deleteData) {
      return false;
    }
    screaming::BitSet::Iterator iter(deleteData->bits);
    int32_t result = iter.advance(docId);
    return result == docId;
  }
  
  bool hasDeletes() const {
    return deleteData != nullptr;
  }
  
  int32_t numDeletedDocs() const {
    return deleteData->numDeleted;
  }
};

/// IndexReader is thread safe
class IndexReader {
public:

  class Segment {
    const std::shared_ptr<PostingsReader> sharedPostingsReader;
    const std::shared_ptr<DeletedDocs> sharedDeletedDocs;


  public:
    struct SegmentInfo {
      uint64_t seg_id = 1;                    // Unique identifier for the segment
      uint64_t deletes_gen = 2;               // What deletes version to use for the segment (0 if no deletes)
      uint64_t min_version = 3;               // Minimum update version in this segment
      uint64_t max_version = 4;               // Maximum update version in this segment
      int32_t  max_doc = 5;                   // Number of documents in this segment (ignoring deletes)
      int32_t  deletes = 6;                   // Number of deleted documents in this segment
    };

    const SegmentInfo segInfo;    // metadata read from the index info file about the segment
    const int64_t base;           // global index (ordinal/rank) of the first document in this segment with respect to the list of segments
    const int32_t ord;                // index of this segment in the list of segments

    Segment(std::shared_ptr<PostingsReader>&& postingsReader, std::shared_ptr<DeletedDocs>&& deletedDocs, 
              SegmentInfo segInfo, int64_t base, int ord)
            :  sharedPostingsReader(std::move(postingsReader)), sharedDeletedDocs(std::move(deletedDocs)),
                segInfo(segInfo), base(base), ord(ord) {
    }

    PostingsReader& postingsReader() const noexcept {
      return *sharedPostingsReader;
    }
    
    const DeletedDocs& deletedDocs() const noexcept {
      return *sharedDeletedDocs;
    }
  };


  IndexReader(Directory& dir) {
    // because old segments could be merged away before we have a chance to read them, we need
    // to check if there is a new index info file and retry the open if so.
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
        // throw exception, or just have zero segments? Or a single segment with no docs?
        IREADER_DEBUG("Empty IndexReader");
      } else {
        try {
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
          IREADER_DEBUG("\tOpening IndexReader, commitTime={}", commitTimeUs);
          
          segs.reserve(indexInfo.segments_size());
          for (int i = 0; i < indexInfo.segments_size(); i++) {
            const auto& segment = indexInfo.segments(i);
            uint64_t segId = segment.seg_id();
            uint64_t deletesGen = segment.deletes_gen();
            int32_t nDocs = segment.max_doc();
            unused(nDocs);
            
            auto postingsReader = std::make_shared<PostingsReader>(dir, segId);
            auto deletedDocs = std::make_shared<DeletedDocs>(dir, segId, deletesGen);

            Segment::SegmentInfo segmentInfo;
            segmentInfo.seg_id = segId;
            segmentInfo.deletes_gen = deletesGen;
            segmentInfo.min_version = segment.min_version();
            segmentInfo.max_version = segment.max_version();
            segmentInfo.max_doc = nDocs;
            segmentInfo.deletes = segment.deletes();

            segs.emplace_back(std::move(postingsReader), std::move(deletedDocs), segmentInfo, maxdoc, i);
            maxdoc += segs.back().postingsReader().numDocs();
            assert(nDocs == segs.back().postingsReader().numDocs());
          }
        } catch (std::filesystem::filesystem_error& e) {
          // if this is the second time we've tried this same commit point, then throw the exception
          if (commitTimeUs > lastCommitTime) {
            IREADER_DEBUG("Error reading IndexReader: {}, will retry.", e.what());
            lastCommitTime = commitTimeUs;
            retry = true;
          } else {
            IREADER_DEBUG("Error reading IndexReader: {}, THROWING ", e.what());
            throw;
          }
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

  int64_t numDocs() const noexcept {
    return maxdoc;
  }

private:
  std::vector<Segment> segs;
  int64_t maxdoc = 0;
  uint64_t commitTimeUs = 0;
};

}