#pragma once
#include <span>
#include "PostingsReader.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define IREADER_DEBUG LOG_TRACE
// #define IREADER_DEBUG LOG_DEBUG

namespace solux {

/// IndexReader is thread safe
class IndexReader {
public:

  class Segment {
    const std::shared_ptr<PostingsReader> sharedPostingsReader;
  public:
    const int64_t base;   // global index (ordinal/rank) of the first document in this segment with respect to the list of segments
    const int ord;        // index of this segment in the list of segments

    Segment(std::shared_ptr<PostingsReader>&& postingsReader, int64_t base, int ord)
            :  sharedPostingsReader(std::move(postingsReader)), base(base), ord(ord) {
    }

    PostingsReader& postingsReader() const noexcept {
      return *sharedPostingsReader;
    }

    // TODO: need deleted docs for this segment. Lazy or not?
  };


  IndexReader(Directory& dir) {
    // because old segments could be merged away before we have a chance to read them, we need
    // to check if there is a new index info file and retry the open if so.
    uint64_t lastCommitTime = 0;
    bool retry;
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
          commitTimeUs = segmentsIs.readLong();
          IREADER_DEBUG("\tOpening IndexReader, commitTime={}", commitTimeUs);
          int nsegs = segmentsIs.readVint();
          segs.reserve(nsegs);
          for (int i = 0; i < nsegs; i++) {
            uint64_t segId = segmentsIs.readVlong();
            int32_t nDocs = segmentsIs.readVint();
            segs.emplace_back(std::make_shared<PostingsReader>(dir, segId), maxdoc, i);
            maxdoc += segs.back().postingsReader().numDocs();
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

  const std::span<Segment> segments() noexcept {
    return segs;
  }

  int64_t numDocs() const noexcept {
    return maxdoc;
  }

private:
  std::vector<Segment> segs;
  int64_t maxdoc = 0;
  int64_t commitTimeUs = 0;
};

}