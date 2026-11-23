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
/// The writer picks SPARSE when its body is at most a quarter of the bitmap, so
/// a generation with few deletes in a large segment costs bytes per delete
/// rather than maxDoc / 8.
class LiveDocsWriter {
public:
  static constexpr uint64_t DENSE = 1;
  static constexpr uint64_t SPARSE = 2;
  static constexpr uint64_t HEADER_BYTES = 24;

  /// Write a liveDocs file and return its size. Throws on failure, including
  /// when numLiveDocs does not match the clear bits (the header would lie) or
  /// when bits past maxDoc are set (dense readers map the words as written).
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

    size_t numWords = screaming::FixedBitSet::sizeInWords(maxDoc);
    // The bits of the last word below maxDoc; the rest is padding.
    uint64_t lastMask = ~0ULL >> (numWords * 64 - (size_t)maxDoc);
    auto dead = [&](size_t w) { return ~liveBits.words[w] & (w + 1 == numWords ? lastMask : ~0ULL); };
    if (numWords > 0 && (liveBits.words[numWords - 1] & ~lastMask)) {
      throw std::logic_error(std::format("liveDocs {} has bits set past maxDoc {}", deleteFileName, maxDoc));
    }
    int64_t numDeleted = 0;
    for (size_t w = 0; w < numWords; w++) numDeleted += std::popcount(dead(w));
    if (numDeleted != maxDoc - numLiveDocs) {
      throw std::logic_error(std::format("liveDocs {} has {} deleted docs, expected {}",
                                         deleteFileName, numDeleted, maxDoc - numLiveDocs));
    }

    bool isSparse = sparse(maxDoc, numLiveDocs);
    uint64_t bytes = HEADER_BYTES + (isSparse ? (uint64_t)numDeleted * sizeof(uint32_t) : numWords * sizeof(uint64_t));
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
      // Native-endian ids, as the reader copies them out.
      uint32_t ids[256];
      size_t n = 0;
      for (size_t w = 0; w < numWords; w++) {
        for (uint64_t bits = dead(w); bits != 0; bits &= bits - 1) {
          ids[n++] = (uint32_t)(w * 64 + std::countr_zero(bits));
          if (n == std::size(ids)) {
            out.write(ids, sizeof(ids));
            n = 0;
          }
        }
      }
      out.write(ids, n * sizeof(uint32_t));
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

private:
  // SPARSE only at a quarter of the bitmap or less: readers still expand it to a full heap bitmap.
  static bool sparse(int32_t maxDoc, int32_t numLive) {
    return 16 * (uint64_t)(maxDoc - numLive) <= screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
  }
};

} // namespace luxir
