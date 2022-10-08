#pragma once
#include "PostingsReader.h"

namespace solux {


// TODO: how to handle multi-reader (say for most common use case of multiple shards in same node?)

// IndexReader is thread safe
class IndexReader {
public:

  class Segment {
    // We could keep this in a separate vector in the IndexReader, it would make our segment array slightly more compact.
    // We have a separate reference preader that doesn't go through the shared_ptr since the IndexReader is supposed to
    // be live while it is being searched.
    std::shared_ptr<PostingsReader> sharedPostingsReader;
  public:
    PostingsReader& preader;
    const int64_t base;   // global index (ordinal/rank) of the first document in this segment with respect to the list of segments
    const int ord;        // index of this segment in the list of segments

    Segment(std::shared_ptr<PostingsReader> postingsReader, int64_t base, int ord)
            :  sharedPostingsReader(postingsReader), preader(*sharedPostingsReader), base(base), ord(ord) {
    }

    // TODO: need deleted docs for this segment. Lazy or not?
  };


  IndexReader(Directory& dir) {
    std::shared_ptr<InputFile> inputFile = dir.openFile(Postings::INDEX_INFO_FILE);
    if (inputFile == nullptr) {
      // throw exception, or just have zero segments?
      LOG_DEBUG("Empty IndexReader");
    } else {
      InputStream segmentsIs = inputFile->getInputStream();
      gen = segmentsIs.readVlong();
      int nsegs = segmentsIs.readVint();
      segs.reserve(nsegs);
      for (int i=0; i<nsegs; i++) {
        auto s = segmentsIs.readStr();
        segs.emplace_back(std::move(std::make_shared<PostingsReader>(dir, s)), maxdoc, i);
        maxdoc += segs.back().preader.numDocs();
      }
    }
  }

  const std::span<Segment> segments() {
    return segs;
  }

  int64_t numDocs() {
    return maxdoc;
  }

private:
  std::vector<Segment> segs;
  uint64_t gen = 0;
  int64_t maxdoc = 0;
};

}