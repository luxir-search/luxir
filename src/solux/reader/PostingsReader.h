#pragma once

#include <cstdint>
#include <assert.h>
#include <iostream>
#include <array>
#include <unordered_map>
#include <vector>
#include <string>
#include <charconv>
#include <filesystem>
#include "solux/store/Directory.h"
#include "solux/store/InputStream.h"
#include "solux/codec/Codec.h"

namespace solux {

class PostingsReader;
class FieldReader;
class TermsEnum;
class DocsEnum;

// Some stuff that the postings reader and writer need to share.
class Postings {
public:
  static constexpr int32_t TERMS_BLOCK_SIZE = 32;
  static constexpr int32_t POSITIONS_BLOCK_SIZE = SoluxPFOR::BLOCK_SIZE;
  static constexpr int32_t DOCS_BLOCK_SIZE =  SoluxPFOR::BLOCK_SIZE;
  static constexpr int32_t NUMERIC_BLOCK_SIZE = 16384;

  // using PositionsCodec = IntegerCODECTypeWrapper<SIMDCompressionLib::FastPFor<4, false>>;
  using PositionsCodec = SoluxPFOR;
  // using DocsCodec = IntegerCODECTypeWrapper<SIMDCompressionLib::SIMDFastPFor<4, SIMDCompressionLib::RegularDeltaSIMD>>;
  using DocsCodec = SoluxPFORd;
  using TFreqCodec = PositionsCodec; // same type, but should also share instances for better performance
  using NumericCodec = SoluxSIMDFor;

  // These could be static if we made them thread safe...
  static DocsCodec docCodec;
  static PositionsCodec posCodec;
  static TFreqCodec& tfreqCodec;
  static NumericCodec numericCodec;

  // Filename related utilities.  We try to keep filenames short for many reasons, including
  // being able to fit in short-string optimization.

  static constexpr std::string_view INDEX_INFO_FILE = "s.olux"; // lists all segments in the index
  static constexpr std::string_view PREFIX_FNAME = "s";         // prefix for all data files
  static constexpr std::string_view SOLUX_HEADER = "SOLUX001";  // every data file starts with this header


  // Create a sortable string from a number.  It's currently
  // a base36 representation prefixed with the number of digits-1 to make it sort correctly.
  // Example: getSortableString(0)->"00", getSortableString(10)->"0a", getSortableString(36)->"110"
  static std::string getSortableString(uint64_t val) {
    std::array<char, 14> arr; // Need 13 digits (log(2**64)/log(36)==12.3) plus one for the length prefix.
    auto start = arr.begin() + 1;  // leave room to write the prefix
    auto[end, ec] = std::to_chars(start, arr.end(), val, 36);
    uint8_t extraDigits = end - start - 1;
    arr[0] = extraDigits <= 9 ? ('0' + extraDigits) : ('a' + (extraDigits - 10));  // base36 prefix
    return std::string(arr.begin(), end);
  }

  static std::string getIndexFileName(const std::string_view gen, const std::string_view suffix) {
    return std::string(PREFIX_FNAME).append(gen).append(suffix);
  }

  static std::string getIndexFileNamePrefix(uint64_t segId) {
    return std::string(PREFIX_FNAME).append(getSortableString(segId));
  }

  /// Filename for the segment given the segment gen/number and the file number.
  /// Example: the 3rd file in the 4th segment produced would be "s04_03"
  static std::string getIndexFileName(const std::string_view gen, uint32_t filenum) {
    std::string s = std::string(PREFIX_FNAME).append(gen);
    s += '_';
    s.append(getSortableString(filenum));
    return s;
  }

  /// A file that contains deletes for the segment.  deleteGen==0 implies no deletes.
  static std::string getDeleteFileName(const std::string_view gen, uint64_t deleteGen) {
    std::string s = std::string(PREFIX_FNAME).append(gen);
    s += '_';
    s += '_'; // use double underscore to prevent clashes with other index filenames
    s.append(getSortableString(deleteGen));
    return s;
  }

};



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