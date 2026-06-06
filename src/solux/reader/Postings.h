#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
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

  // Reserved field name under which the default stored-fields resource is
  // registered in a segment's per-field index.  Named column families live
  // in this namespace too (e.g. "_stored_paragraphs_").
  static constexpr std::string_view STORED_DEFAULT_RESOURCE = "_stored_";


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

  // Parse a sortable string back to a number. Inverse of getSortableString.
  // Returns 0 on failure.
  static uint64_t parseSortableString(std::string_view s) {
    if (s.size() < 2) return 0;
    s.remove_prefix(1);  // skip the length-prefix char
    uint64_t val = 0;
    std::from_chars(s.data(), s.data() + s.size(), val, 36);
    return val;
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

  /// Filename for an aux-index file (see AuxIndexInfo in solux_types.proto).
  /// Format: "s_<name>_<sortable_gen>_<sortable_fnum>".  The leading "s_" prefix
  /// disambiguates from segment files (which are "s<base36>" without an underscore
  /// after the prefix).  <name> is opaque to the file layer; callers conventionally
  /// use "<kind_short>.<field>" (e.g. "vec.title_v") so files group naturally on ls.
  static std::string getAuxIndexFileName(std::string_view name, uint64_t gen, uint32_t fnum) {
    std::string s(PREFIX_FNAME);
    s += '_';
    s.append(name);
    s += '_';
    s.append(getSortableString(gen));
    s += '_';
    s.append(getSortableString(fnum));
    return s;
  }

  /// Filename for a segment-local overlay file (see SegmentInfo.overlays).
  /// Format: "s<segId>__<name>_<gen>_<fnum>" - the segment prefix groups a
  /// segment's overlays with its data files and liveDocs in ls, and lets the
  /// existing dead-segment deletePrefix sweep reclaim them automatically.
  /// <name> conventionally contains a dot ("vec.title_v"), so it cannot
  /// collide with the liveDocs "__L" marker.  Parsing, if ever needed, is
  /// END-anchored (the last two underscore-separated fields are gen and
  /// fnum) because field names may themselves contain underscores and
  /// digits.  Filenames are opaque to the file layer; the IndexInfo manifest
  /// binds file <-> entry.  gen is an overlay-defined generation.  Vector
  /// overlays use a per-(segment, field) rebuild ordinal, so rebuilds write a
  /// new file while old readers still hold the previous file.
  static std::string getSegmentOverlayFileName(uint64_t segId, std::string_view name,
                                               uint64_t gen, uint32_t fnum) {
    std::string s(PREFIX_FNAME);
    s.append(getSortableString(segId));
    s += "__";
    s.append(name);
    s += '_';
    s.append(getSortableString(gen));
    s += '_';
    s.append(getSortableString(fnum));
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
