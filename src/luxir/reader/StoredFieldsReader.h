// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "IntColReader.h"
#include "Postings.h"
#include "PostingsReader.h"
#include "StoredChunkCodec.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/StrRef.h"

namespace luxir {

///
/// Reader for stored fields written by StoredFieldsWriter.
/// See StoredFieldsWriter.h for the on-disk layout.
///
/// Usage:
///   StoredFieldsReader reader(postingsReader, segFieldInfo);
///   reader.readDoc(docID,
///     [](std::string_view name, std::span<const std::string_view> values) {
///       // receives one callback per field; values has one entry for
///       // single-valued fields and N entries for multi-valued fields
///     });
///
/// Callback-lifetime contract (applies to readDoc, readField, readFieldById):
///   - The fieldName string_view passed to readDoc points into the reader's
///     PostingsReader's cached metadata and is valid for the reader's lifetime. It is
///     safe to keep a copy of the string_view for as long as the reader
///     exists.
///   - The values span (std::span<const std::string_view>) is a view into
///     the reader's internal valueSpanScratch buffer and is only valid for
///     the duration of the current callback invocation.  The next callback
///     (or any subsequent read* call) reuses this buffer.
///   - Each value std::string_view inside the span points into the reader's
///     decompressed chunk buffer, which stays valid for the current
///     readDoc/readField call only.  A subsequent read* call that targets a
///     different chunk will overwrite those bytes.  Callers that need the
///     value beyond the callback must copy into their own storage.
///
/// Not thread-safe.
///
class StoredFieldsReader {
private:
  const StoredFieldsMetadata* metadata;
  InputStream chunkIS;
  int64_t chunksStart = 0;
  int64_t chunksRegionEnd = 0;
  int32_t numChunks_ = 0;
  int32_t maxDoc_ = 0;
  std::optional<MonoReader> firstDocCol;     // chunkN -> firstDocID
  std::optional<MonoReader> chunkOffsetCol;  // chunkN -> file offset (numChunks+1 entries)

  std::vector<char> scratch;
  std::vector<std::string_view> valueSpanScratch;
  int32_t cachedChunkNum = -1;
  StoredChunkDecompressor decompressor;

  struct ChunkLookup {
    int32_t chunkNum;
    int32_t firstDocID;
  };

public:
  StoredFieldsReader(PostingsReader& postingsReader, const PostingsReader::Field& fieldInfo)
    : metadata(fieldInfo.stored.get()),
      maxDoc_(postingsReader.maxDoc())
  {
    assert(fieldInfo.type == FieldType::BIN);
    assert(fieldInfo.numValues > 0);
    assert(metadata != nullptr);

    chunkIS = postingsReader.getInputStream(fieldInfo.columnLoc.filenum());
    chunksStart = (int64_t)fieldInfo.columnLoc.offset();
    chunksRegionEnd = chunksStart + fieldInfo.columnMetaOff;
    numChunks_ = (int32_t)fieldInfo.numValues;

    firstDocCol.emplace(postingsReader, fieldInfo.monoLoc, fieldInfo.monoMetaOff, numChunks_);
    chunkOffsetCol.emplace(postingsReader, fieldInfo.mono2Loc, fieldInfo.mono2MetaOff, numChunks_ + 1);
  }

  // Invokes callback once for each stored-field entry on docID:
  //
  //   callback(std::string_view fieldName,
  //            std::span<const std::string_view> values)
  //
  // fieldName is the stored field's name and is valid for the reader's
  // lifetime.  values holds one entry for single-valued fields and N
  // entries for multi-valued fields; the span and each string_view inside
  // it are only valid during the callback (see the class-level lifetime
  // contract).  Docs with nothing stored produce no callbacks.
  template <class Callback>
  void readDoc(int32_t docID, Callback&& callback) {
    assert(docID >= 0 && docID < maxDoc_);

    auto [chunkNum, firstDocInChunk] = findChunk(docID);
    int32_t firstDocInNext = (chunkNum + 1 < numChunks_)
        ? (int32_t)firstDocCol->valueAt(chunkNum + 1)
        : maxDoc_;
    int32_t numDocsInChunk = firstDocInNext - firstDocInChunk;
    int32_t docIndex = docID - firstDocInChunk;
    assert(docIndex >= 0 && docIndex < numDocsInChunk);

    decompressChunk(chunkNum);

    const char* buf = scratch.data();
    const int32_t* offsets = reinterpret_cast<const int32_t*>(buf);
    int32_t docStart = offsets[docIndex];
    int32_t docEnd = (docIndex + 1 < numDocsInChunk)
        ? offsets[docIndex + 1]
        : (int32_t)scratch.size();
    assert(docStart <= docEnd);

    const char* p = buf + docStart;
    const char* pend = buf + docEnd;
    uint32_t numFields = InputStream::readVint(p, pend);
    for (uint32_t i = 0; i < numFields; i++) {
      uint32_t fid = InputStream::readVint(p, pend);
      uint32_t numValues = InputStream::readVint(p, pend);
      assert(fid < metadata->fieldNames.size());
      std::string_view fieldName = metadata->fieldNames[fid];
      valueSpanScratch.resize(0);
      valueSpanScratch.reserve(numValues);
      for (uint32_t v = 0; v < numValues; v++) {
        uint32_t length = InputStream::readVint(p, pend);
        valueSpanScratch.emplace_back(p, length);
        p += length;
      }
      callback(fieldName, std::span<const std::string_view>(valueSpanScratch));
    }
    assert(p == pend);
  }

  // Returns the segment-local field id for fieldName, or -1 if this
  // resource has no entries for that field.  Callers iterating over many
  // docs should resolve the fid once and use readFieldById to avoid a
  // per-call linear scan over the field table.
  int32_t fieldId(std::string_view fieldName) const {
    return findFieldId(fieldName);
  }

  // Does this resource contain any entries for fieldName?
  bool hasField(std::string_view fieldName) const {
    return findFieldId(fieldName) >= 0;
  }

  // Read only the entries for fieldName in docID.  See readFieldById for
  // the callback signature and semantics; this overload resolves the fid
  // by name on every call. Use readFieldById in hot loops to avoid the
  // per-call linear scan over the field table.
  template <class Callback>
  bool readField(int32_t docID, std::string_view fieldName, Callback&& callback) {
    int32_t targetFid = findFieldId(fieldName);
    if (targetFid < 0) return false;
    return readFieldById(docID, targetFid, std::forward<Callback>(callback));
  }

  // Read only the entries for field `targetFid` in docID, invoking:
  //
  //   callback(std::span<const std::string_view> values)
  //
  // once for each entry.  A field can appear multiple times in one doc
  // (one callback per entry, matching the grouping produced by the
  // writer); most callers just see a single invocation.  The values span
  // and the string_views inside it are only valid during the callback
  // (see the class-level lifetime contract).  Returns true if at least
  // one entry was found.  Skips non-matching fields without materializing
  // value views - cheap to call even on docs with many stored fields.
  template <class Callback>
  bool readFieldById(int32_t docID, int32_t targetFid, Callback&& callback) {
    assert(docID >= 0 && docID < maxDoc_);
    assert(targetFid >= 0 && (size_t)targetFid < metadata->fieldNames.size());

    auto [chunkNum, firstDocInChunk] = findChunk(docID);
    int32_t firstDocInNext = (chunkNum + 1 < numChunks_)
        ? (int32_t)firstDocCol->valueAt(chunkNum + 1)
        : maxDoc_;
    int32_t numDocsInChunk = firstDocInNext - firstDocInChunk;
    int32_t docIndex = docID - firstDocInChunk;
    assert(docIndex >= 0 && docIndex < numDocsInChunk);

    decompressChunk(chunkNum);

    const char* buf = scratch.data();
    const int32_t* offsets = reinterpret_cast<const int32_t*>(buf);
    int32_t docStart = offsets[docIndex];
    int32_t docEnd = (docIndex + 1 < numDocsInChunk)
        ? offsets[docIndex + 1]
        : (int32_t)scratch.size();

    const char* p = buf + docStart;
    const char* pend = buf + docEnd;
    uint32_t numFields = InputStream::readVint(p, pend);
    bool any = false;
    for (uint32_t i = 0; i < numFields; i++) {
      uint32_t fid = InputStream::readVint(p, pend);
      uint32_t numValues = InputStream::readVint(p, pend);
      if ((int32_t)fid != targetFid) {
        // Skip past value bytes without populating the scratch buffer.
        for (uint32_t v = 0; v < numValues; v++) {
          uint32_t length = InputStream::readVint(p, pend);
          p += length;
        }
        continue;
      }
      valueSpanScratch.resize(0);
      valueSpanScratch.reserve(numValues);
      for (uint32_t v = 0; v < numValues; v++) {
        uint32_t length = InputStream::readVint(p, pend);
        valueSpanScratch.emplace_back(p, length);
        p += length;
      }
      callback(std::span<const std::string_view>(valueSpanScratch));
      any = true;
    }
    return any;
  }

  int32_t numChunks() const { return numChunks_; }

  // Encoded document views borrow the one decompressed chunk. Returning false
  // from the callback stops the visit; views expire on the next chunk read.
  template <class Callback>
  void readChunkDocs(int32_t chunkNum, Callback&& callback) {
    assert(chunkNum >= 0 && chunkNum < numChunks_);
    int32_t first = (int32_t)firstDocCol->valueAt(chunkNum);
    int32_t end = chunkNum + 1 < numChunks_ ? (int32_t)firstDocCol->valueAt(chunkNum + 1) : maxDoc_;
    decompressChunk(chunkNum);
    const int32_t* offsets = (const int32_t*)scratch.data();
    for (int32_t doc = first; doc < end; doc++) {
      int32_t start = offsets[doc - first];
      int32_t limit = doc + 1 < end ? offsets[doc + 1 - first] : (int32_t)scratch.size();
      if (!callback(doc, std::string_view(scratch.data() + start, (size_t)(limit - start)))) return;
    }
  }

  int32_t maxDoc() const { return maxDoc_; }
  int64_t maxChunkBytes() const { return metadata->maxChunkBytes; }
  // Chunks sealed before reaching the chunk limits (see StoredFieldsWriter).
  int32_t numDirtyChunks() const { return metadata->numDirtyChunks; }
  // Codec and chunk limits this resource was written with.
  const StoredChunkFormat& format() const { return metadata->format; }
  // The trained ZSTD dictionary every chunk was compressed with, or empty.
  // The view stays valid for as long as the PostingsReader is alive.
  std::string_view dictionary() const { return metadata->dictionary; }

  // Every field name this resource holds, in segment-local field-id order.
  // The views point into the resource's metadata in the segment file, so they
  // stay valid for as long as the PostingsReader this reader was opened on is
  // alive - beyond this reader object itself.  Merges also use this table to
  // decide whether compressed chunk bodies (which reference these ids) can be
  // copied verbatim.
  std::span<const std::string_view> fieldNames() const { return metadata->fieldNames; }

  // Raw access to the contiguous chunks region ([int32 uncompressedSize]
  // [compressed bytes] per chunk, metadata excluded), for verbatim copy by
  // merges.
  const char* chunkRegionPtr() const { return chunkIS.ptr(chunksStart); }
  int64_t chunkRegionBytes() const { return chunksRegionEnd - chunksStart; }

  // Copy out the chunk directory: per-chunk first docID and file offset
  // relative to the region start (the mono2 sentinel is not included).
  void readChunkDirectory(std::vector<int64_t>& firstDocs,
                          std::vector<int64_t>& fileOffsets) const {
    firstDocs.resize((size_t)numChunks_);
    fileOffsets.resize((size_t)numChunks_);
    for (int32_t i = 0; i < numChunks_; i++) {
      firstDocs[(size_t)i] = firstDocCol->valueAt(i);
      fileOffsets[(size_t)i] = chunkOffsetCol->valueAt(i);
    }
  }

  // Cheap metadata-only read for merge admission.  Does not allocate or build
  // chunk offset readers; it only peeks at the stored-fields metadata header.
  static int64_t peekMaxChunkBytes(PostingsReader& postingsReader,
                                   const SegFieldInfo& fieldInfo) {
    assert(fieldInfo.type == FieldType::BIN);
    InputStream chunkIS = postingsReader.getInputStream(fieldInfo.columnLoc.filenum());
    int64_t metadataOffset = (int64_t)fieldInfo.columnLoc.offset() + fieldInfo.columnMetaOff;
    const char* metaPtr = chunkIS.ptr(metadataOffset);
    const char* end = chunkIS.ptr(chunkIS.size());
    return (int64_t)InputStream::readVlong(metaPtr, end);
  }

private:
  // Linear scan of the field table; typical segments have O(10) stored fields, so
  // a map isn't worth the overhead.
  int32_t findFieldId(std::string_view fieldName) const {
    for (size_t i = 0; i < metadata->fieldNames.size(); i++) {
      if (metadata->fieldNames[i] == fieldName) return (int32_t)i;
    }
    return -1;
  }

  // Interpolation search for the largest chunkNum where
  // firstDocCol[chunkNum] <= docID.  Chunk size is variable (capped by
  // uncompressed bytes or doc count), but within a single segment the
  // docs-per-chunk distribution is usually narrow, so a proportion-based
  // first guess lands close to the correct chunk and the linear adjust
  // usually walks zero or one steps.  MonoReader's valueAt is O(1) per
  // probe but not free (block decode + interpolation), so we only read it
  // on the probes we actually need.  Worst case (highly skewed doc sizes)
  // degrades to linear scan from the first guess.
  //
  // Returns both the chunk number and the chunk's first docID, which the
  // caller uses without a redundant valueAt().
  ChunkLookup findChunk(int32_t docID) const {
    assert(numChunks_ > 0);
    int32_t guess = (int32_t)((int64_t)docID * numChunks_ / maxDoc_);
    if (guess >= numChunks_) guess = numChunks_ - 1;
    if (guess < 0) guess = 0;

    int32_t firstDoc = (int32_t)firstDocCol->valueAt(guess);
    if (firstDoc <= docID) {
      // Walk forward while the next chunk's first doc is also <= docID.
      // If we advance at all, the new position was already confirmed by
      // this very probe - no need to verify backward afterwards.
      while (guess + 1 < numChunks_) {
        int32_t nextFirst = (int32_t)firstDocCol->valueAt(guess + 1);
        if (nextFirst > docID) break;
        guess++;
        firstDoc = nextFirst;
      }
    } else {
      // Initial guess was too high; walk backward until firstDoc <= docID.
      do {
        assert(guess > 0);  // chunk 0's firstDoc is 0 <= any valid docID
        guess--;
        firstDoc = (int32_t)firstDocCol->valueAt(guess);
      } while (firstDoc > docID);
    }
    return {guess, firstDoc};
  }

  void decompressChunk(int32_t chunkNum) {
    if (chunkNum == cachedChunkNum) {
      return;
    }
    int64_t off = chunkOffsetCol->valueAt(chunkNum);
    int64_t nextOff = chunkOffsetCol->valueAt(chunkNum + 1);

    const char* chunkPtr = chunkIS.ptr(chunksStart + off);
    int32_t uncompressedSize;
    memcpy(&uncompressedSize, chunkPtr, sizeof(int32_t));
    chunkPtr += sizeof(int32_t);
    int32_t compressedSize = (int32_t)(nextOff - off) - (int32_t)sizeof(int32_t);

    scratch.resize((size_t)uncompressedSize);
    decompressor.decompress(metadata->format.codec, metadata->ddict ? &*metadata->ddict : nullptr,
                            chunkPtr, (size_t)compressedSize,
                            scratch.data(), scratch.size());
    cachedChunkNum = chunkNum;
  }
};

}  // namespace luxir
