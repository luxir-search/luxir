#pragma once

#include "solux/store/Directory.h"
#include "solux/store/OutputStream.h"
#include "solux/util/screaming.h"
#include "solux/reader/Postings.h"

namespace solux {

/// Utility class for writing live docs bitmap files.
class LiveDocsWriter {
public:
  /// Write a liveDocs bitmap file. Returns true on success.
  /// @param dir Directory to write the file to
  /// @param segId Segment ID
  /// @param liveGen Live docs generation (must be > 0)
  /// @param liveBits The bitmap of live documents
  /// @param maxDoc Maximum document ID (total documents in segment)
  /// @param numLiveDocs Number of live documents (bits set in liveBits)
  static bool writeLiveDocs(Directory& dir, uint64_t segId, uint64_t liveGen,
                                   const screaming::FixedBitSet& liveBits, int32_t maxDoc, int32_t numLiveDocs) {
    assert(liveGen > 0 && numLiveDocs >= 0 && numLiveDocs <= maxDoc);
    if (liveGen == 0) {
      return false; // liveGen must be > 0
    }
    
    std::string segStr = Postings::getSortableString(segId);
    std::string deleteFileName = Postings::getLiveDocsFileName(segStr, liveGen);
    
    auto deleteFile = dir.createFile(deleteFileName);
    if (!deleteFile) {
      return false;
    }
    
    OutputStream out(deleteFile.get());
    
    // Write header - this is the standard format for live docs files
    out.writeBytes(Postings::SOLUX_HEADER); // "SOLUX001"
    out.writeLong(1); // format version
    out.writeInt(maxDoc);
    out.writeInt(numLiveDocs);
    
    // Write the live docs bitset data (already 64-bit aligned after 24-byte header)
    size_t bitsDataSize = screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
    out.write(liveBits.words, bitsDataSize);
    out.close();
    
    dir.finishFile(*deleteFile);
    
    return true;
  }
};

} // namespace solux