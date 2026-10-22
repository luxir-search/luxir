// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/store/Directory.h"
#include "luxir/store/FileIOException.h"
#include "luxir/store/OutputStream.h"
#include "luxir/util/screaming.h"
#include "luxir/reader/Postings.h"

namespace luxir {

/// Utility class for writing live docs bitmap files.
class LiveDocsWriter {
public:
  /// Write a liveDocs bitmap file. Throws on failure.
  /// @param dir Directory to write the file to
  /// @param segId Segment ID
  /// @param liveGen Live docs generation (must be > 0)
  /// @param liveBits The bitmap of live documents
  /// @param maxDoc Maximum document ID (total documents in segment)
  /// @param numLiveDocs Number of live documents (bits set in liveBits)
  static void writeLiveDocs(Directory& dir, uint64_t segId, uint64_t liveGen,
                                   const screaming::FixedBitSet& liveBits, int32_t maxDoc, int32_t numLiveDocs, std::vector<FileDescriptor>* descriptors = nullptr) {
    assert(liveGen > 0 && numLiveDocs >= 0 && numLiveDocs <= maxDoc);
    if (liveGen == 0) {
      throw std::invalid_argument("liveDocs generation must be > 0");
    }

    std::string segStr = Postings::getSortableString(segId);
    std::string deleteFileName = Postings::getLiveDocsFileName(segStr, liveGen);

    Directory::FileCreateOptions options;
    options.expectedSize = 24 + screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
    auto deleteFile = dir.createFile(deleteFileName, options);
    if (!deleteFile) {
      throw FileIOException("Failed to create liveDocs file " + deleteFileName);
    }

    OutputStream out(deleteFile.get());

    // Write header - this is the standard format for live docs files
    out.writeBytes(Postings::LUXIR_HEADER);
    out.writeLong(1); // format version
    out.writeInt(maxDoc);
    out.writeInt(numLiveDocs);

    // Write the live docs bitset data (already 64-bit aligned after 24-byte header)
    size_t bitsDataSize = screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
    out.write(liveBits.words, bitsDataSize);
    out.close();

    dir.finishFile(*deleteFile);
    if (descriptors) descriptors->push_back(deleteFile->descriptor());
  }

  // Overload that also appends the written filename for fsync at commit time.
  static void writeLiveDocs(Directory& dir, uint64_t segId, uint64_t liveGen,
                            const screaming::FixedBitSet& liveBits, int32_t maxDoc, int32_t numLiveDocs,
                            std::vector<std::string>& filenames, std::vector<FileDescriptor>* descriptors = nullptr) {
    std::string segStr = Postings::getSortableString(segId);
    std::string deleteFileName = Postings::getLiveDocsFileName(segStr, liveGen);
    writeLiveDocs(dir, segId, liveGen, liveBits, maxDoc, numLiveDocs, descriptors);
    filenames.push_back(std::move(deleteFileName));
  }
};

} // namespace luxir
