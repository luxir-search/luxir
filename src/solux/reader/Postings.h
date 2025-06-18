#pragma once

#include <cstdint>
#include <string>
#include <array>

namespace solux {


// Some postings info that the postings reader and writer need to share.
class Postings {
public:
  static constexpr int32_t TERMS_BLOCK_SIZE = 32;
  static constexpr int32_t POSITIONS_BLOCK_SIZE = 128;
  static constexpr int32_t DOCS_BLOCK_SIZE =  128;
  static constexpr int32_t NUMERIC_BLOCK_SIZE = 16384;

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
  static std::string getLiveDocsFileName(const std::string_view gen, uint64_t liveGen) {
    std::string s = std::string(PREFIX_FNAME).append(gen);
    // double underscore to avoid name conflicts with other seg files.
    // Capital L instead of lowercase l because fonts suck.
    s += "__L";
    s.append(getSortableString(liveGen));
    return s;
  }
};

}