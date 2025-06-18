#pragma once

#include <vector>
#include "solux/store/Directory.h"
#include "solux/store/InputStream.h"

namespace solux {

class PostingsReader;
class FieldReader;
class TermsEnum;
class DocsEnum;


// Lowest level postings reader class that needs to correspond to the PostingsWriter class that created the data.
// PostingsReader should be thread-safe at the top level, but any iterators it supplies would not be.
// This does not contain deleted docs, so instances can be shared by different index versions.
class PostingsReader {
  std::vector<std::shared_ptr<InputFile>> files;  // keeps files live while this PostingsReader is live.
  std::vector<InputStream> inputStreams;
  int64_t segInfoOffset;  // after this is segInfo, before this is the field index
  int32_t maxdoc;
public:

  // used as a sentinel value for docs and positions iterators in a single segment.
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  // Static factory method to create PostingsReader with optional handling of missing files.
  // Returns nullptr if missingFileOK=true and any required files are missing.
  static std::shared_ptr<PostingsReader> create(Directory& dir, uint64_t segId, bool missingFileOK = false);

  explicit PostingsReader(Directory& dir, uint64_t segId);

private:
  // Private default constructor for factory method
  PostingsReader() = default;

  // Initialize from files, returns false if missing files and missingFileOK=true
  bool initializeFromFiles(Directory& dir, uint64_t segId, bool missingFileOK);

public:

  int32_t numDocs() const noexcept {
    return maxdoc;
  }

  InputFile* getFile(uint32_t fnum) {
    assert(fnum < files.size());
    return files[fnum].get();
  }

  // We can't get & cache the InputStream in PostingsReader unless we create them all in the constructor (for thread safety)
  // But if we're using mmap, that's probably fine?  Would not be fine if we need to read everything in the constructor.
  // TODO: could also have a mode that opens on demand (and hence synchronizes)... that would be good for something like IndexWriter
  // that needs to only read the ID field to handle overwrites / deletions.  That file *might* already be open by another IndexReader
  // though?  How to coordinate?
  InputStream getInputStream(uint32_t fnum) {
    assert(fnum < inputStreams.size());
    return inputStreams[fnum];
  }

  // return an InputStream positioned on the current location specified
  InputStream getInputStreamSeek(seg_location sloc) {
    InputStream is = getInputStream(sloc.filenum());
    is.seek(sloc.offset());
    return is;
  }

  friend class FieldReader;
};



} // end namespace