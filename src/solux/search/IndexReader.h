#pragma once
#include <span>
#include "PostingsReader.h"

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
    std::shared_ptr<InputFile> inputFile = dir.openFile(Postings::INDEX_INFO_FILE);
    if (inputFile == nullptr) {
      // throw exception, or just have zero segments? Or a single segment with no docs?
      LOG_DEBUG("Empty IndexReader");
    } else {
      InputStream segmentsIs = inputFile->getInputStream();
      gen = segmentsIs.readVlong();
      int nsegs = segmentsIs.readVint();
      segs.reserve(nsegs);
      for (int i=0; i<nsegs; i++) {
        uint64_t segId = segmentsIs.readVlong();
        int32_t nDocs = segmentsIs.readVint();
        segs.emplace_back(std::make_shared<PostingsReader>(dir, segId), maxdoc, i);
        maxdoc += segs.back().postingsReader().numDocs();
      }
    }
  }

  // TODO: implement postingsReader sharing by passing in another IndexReader for reference.

  // The version of the index. Every time an index changes, it's generation number increases by at least 1.
  uint64_t generation() const noexcept {
    return gen;
  }

  const std::span<Segment> segments() noexcept {
    return segs;
  }

  int64_t numDocs() const noexcept {
    return maxdoc;
  }

private:
  std::vector<Segment> segs;
  uint64_t gen = 0;
  int64_t maxdoc = 0;
};

}