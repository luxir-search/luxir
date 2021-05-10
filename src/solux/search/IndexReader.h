#pragma once
#include "PostingsReader.h"

namespace solux {


// TODO: how to handle multi-reader (say for most common use case of multiple shards in same node?)

// IndexReader is thread safe
class IndexReader {
public:
  class Segment {
  public:
    int64_t base;
    int ord;  // index of this segment in the list of segments
    std::unique_ptr<PostingsReader> reader;

    Segment(int64_t base, int ord, std::unique_ptr<PostingsReader>&& postingsReader)
            : base(base), ord(ord), reader(std::move(postingsReader)) {
    }

    /* couldn't get any of these to work with storing directly in vector (when including PostingsReader directly)
     so changed to unique_ptr for now.
    Segment(int64_t base, int ord, Directory& dir, const std::string_view& gen)
            : base(base), ord(ord), reader(dir, gen) {}

    Segment(Segment&&) = default;

    template <typename... Args>
    Segment(int64_t base, int ord, Args&&... args)
      : base(base), ord(ord), reader(std::forward<Args>(args)...) {}  // error: call to implicitly deleted copy constructor
    */
  };


  IndexReader(Directory& dir) {
    std::shared_ptr<InputFile> inputFile = dir.openFile(Postings::INDEX_INFO_FILE);
    if (inputFile == nullptr) {
      // throw exception, or just have zero segments?
    } else {
      InputStream segmentsIs = inputFile->getInputStream();
      gen = segmentsIs.readVlong();
      int nsegs = segmentsIs.readVint();
      segs.reserve(nsegs);
      for (int i=0; i<nsegs; i++) {
        auto s = segmentsIs.readStr();
        segs.emplace_back(maxdoc, i, std::make_unique<PostingsReader>(dir, s));
        maxdoc += segs.back().reader->maxDoc();
      }
      // TODO: sort segments by maxdoc, largest first?  Or make IndexWriter do this when writing segments file?
    }
  }

  const std::vector<Segment>& segments() {
    return segs;
  }

  int64_t maxDoc() {
    return maxdoc;
  }

private:
  // TODO: how to handle deleted docs?  Have a different postings reader that actually knows it's own deleted docs
  // or share a postings reader for all index versions that use it, and keep livedocs at a higher level?
  // If we start caching anything on PostingsReader, we would want the latter (but that would require using shared_ptr again too.
  std::vector<Segment> segs;
  uint64_t gen;
  int64_t maxdoc = 0;
};

}