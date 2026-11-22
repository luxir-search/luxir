// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/store/Directory.h"
#include "luxir/store/FileIOException.h"
#include "luxir/store/OutputStream.h"
#include "luxir/util/screaming.h"
#include "luxir/reader/Postings.h"

#include <bit>
#include <format>
#include <stdexcept>
#include <vector>

namespace luxir {

/// Utility class for writing live docs files.
///
/// A live docs file is a 24-byte header (the 8-byte LUXIR header, a 64-bit
/// kind, int32 maxDoc, int32 numLive) followed by a body in one of two kinds:
///   DENSE:  the live bitmap, sizeInWords(maxDoc) little-endian uint64 words
///           (64-bit aligned after the header), mapped in place by readers.
///   SPARSE: the deleted doc ids, maxDoc - numLive strictly increasing uint32s.
///           Readers expand it into an owned bitmap.
/// The writer picks SPARSE whenever its body is smaller than the bitmap, so a
/// generation with few deletes in a large segment costs bytes per delete
/// rather than maxDoc / 8.
class LiveDocsWriter {
public:
  static constexpr uint64_t DENSE = 1;
  static constexpr uint64_t SPARSE = 2;
  static constexpr uint64_t HEADER_BYTES = 24;

  static bool sparse(int32_t maxDoc, int32_t numLive) {
    return 4 * (uint64_t)(maxDoc - numLive) < screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
  }

  /// The size of a liveDocs file with numLive of maxDoc docs live.
  static uint64_t fileBytes(int32_t maxDoc, int32_t numLive) {
    if (sparse(maxDoc, numLive)) return HEADER_BYTES + 4 * (uint64_t)(maxDoc - numLive);
    return HEADER_BYTES + screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
  }

  /// Write a liveDocs file and return its size. Throws on failure, including
  /// when numLiveDocs does not match the clear bits (the header would lie).
  /// @param dir Directory to write the file to
  /// @param segId Segment ID
  /// @param liveGen Live docs generation (must be > 0)
  /// @param liveBits The bitmap of live documents
  /// @param maxDoc Maximum document ID (total documents in segment)
  /// @param numLiveDocs Number of live documents (bits set in liveBits)
  static uint64_t writeLiveDocs(Directory& dir, uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen, uint64_t writerEpoch,
                                   const screaming::FixedBitSet& liveBits, int32_t maxDoc, int32_t numLiveDocs, std::vector<FileDescriptor>* descriptors = nullptr) {
    assert(liveGen > 0 && numLiveDocs >= 0 && numLiveDocs <= maxDoc);
    if (liveGen == 0) {
      throw std::invalid_argument("liveDocs generation must be > 0");
    }

    std::string segStr = Postings::segmentKey(segId, creatorEpoch);
    std::string deleteFileName = Postings::getLiveDocsFileName(segStr, liveGen, writerEpoch);

    bool isSparse = sparse(maxDoc, numLiveDocs);
    size_t numWords = screaming::FixedBitSet::sizeInWords(maxDoc);
    // One pass counts the clear bits of either kind, and collects them for SPARSE.
    std::vector<uint32_t> deleted;
    if (isSparse) deleted.reserve(maxDoc - numLiveDocs);
    int64_t numDeleted = 0;
    for (size_t w = 0; w < numWords; w++) {
      uint64_t dead = ~liveBits.words[w];
      if (w == numWords - 1 && (maxDoc & 63)) dead &= (1ULL << (maxDoc & 63)) - 1;
      numDeleted += std::popcount(dead);
      if (!isSparse) continue;
      while (dead) {
        deleted.push_back((uint32_t)(w * 64 + std::countr_zero(dead)));
        dead &= dead - 1;
      }
    }
    if (numDeleted != maxDoc - numLiveDocs) {
      throw std::logic_error(std::format("liveDocs {} has {} deleted docs, expected {}",
                                         deleteFileName, numDeleted, maxDoc - numLiveDocs));
    }

    uint64_t bytes = fileBytes(maxDoc, numLiveDocs);
    Directory::FileCreateOptions options;
    options.expectedSize = bytes;
    auto deleteFile = dir.createFile(deleteFileName, options);
    if (!deleteFile) {
      throw FileIOException("Failed to create liveDocs file " + deleteFileName);
    }

    OutputStream out(deleteFile.get());

    out.writeBytes(Postings::LUXIR_HEADER);
    out.writeLong((int64_t)(isSparse ? SPARSE : DENSE));
    out.writeInt(maxDoc);
    out.writeInt(numLiveDocs);

    if (isSparse) {
      out.write(deleted.data(), deleted.size() * sizeof(uint32_t));
    } else {
      out.write(liveBits.words, numWords * sizeof(uint64_t));
    }
    out.close();

    dir.finishFile(*deleteFile);
    if (descriptors) descriptors->push_back(deleteFile->descriptor());
    return bytes;
  }

  // Overload that also appends the written filename for fsync at commit time.
  static uint64_t writeLiveDocs(Directory& dir, uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen, uint64_t writerEpoch,
                                const screaming::FixedBitSet& liveBits, int32_t maxDoc, int32_t numLiveDocs,
                                std::vector<std::string>& filenames, std::vector<FileDescriptor>* descriptors = nullptr) {
    std::string segStr = Postings::segmentKey(segId, creatorEpoch);
    std::string deleteFileName = Postings::getLiveDocsFileName(segStr, liveGen, writerEpoch);
    uint64_t bytes = writeLiveDocs(dir, segId, creatorEpoch, liveGen, writerEpoch, liveBits, maxDoc, numLiveDocs, descriptors);
    filenames.push_back(std::move(deleteFileName));
    return bytes;
  }
};

} // namespace luxir
