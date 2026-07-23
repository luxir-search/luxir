#pragma once

#include <lz4.h>
#include <string_view>
#include <vector>

#include "FieldReader.h"
#include "IntColReader.h"
#include "Postings.h"
#include "PostingsReader.h"
#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"

namespace solux {

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
///     own fieldNames_ storage and is valid for the reader's lifetime.  It is
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
  InputStream chunkIS;
  int64_t chunksStart = 0;
  int64_t chunksRegionEnd = 0;
  int32_t numChunks_ = 0;
  int32_t maxDoc_ = 0;
  int64_t maxChunkBytes_ = 0;

  std::vector<std::string_view> fieldNames_;

  std::optional<MonoReader> firstDocCol;     // chunkN -> firstDocID
  std::optional<MonoReader> chunkOffsetCol;  // chunkN -> file offset (numChunks+1 entries)

  std::vector<char> scratch;
  std::vector<std::string_view> valueSpanScratch;
  int32_t cachedChunkNum = -1;

  struct ChunkLookup {
    int32_t chunkNum;
    int32_t firstDocID;
  };

public:
  StoredFieldsReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo)
    : maxDoc_(postingsReader.maxDoc())
  {
    assert(fieldInfo.type == FieldType::BIN);
    assert(fieldInfo.numValues > 0);

    chunkIS = postingsReader.getInputStream(fieldInfo.columnLoc.filenum());
    chunksStart = (int64_t)fieldInfo.columnLoc.offset();
    chunksRegionEnd = chunksStart + fieldInfo.columnMetaOff;
    numChunks_ = (int32_t)fieldInfo.numValues;

    // Read metadata block (resource stats and field names) which sits at
    // chunksStart + columnMetaOff.
    const char* metaPtr = chunkIS.ptr(chunksRegionEnd);
    const char* end = chunkIS.ptr(chunkIS.size());
    maxChunkBytes_ = (int64_t)InputStream::readVlong(metaPtr, end);
    uint32_t numFields = InputStream::readVint(metaPtr, end);
    fieldNames_.reserve(numFields);
    for (uint32_t i = 0; i < numFields; i++) {
      // PackedTerm layout: one-byte length followed by the string bytes.  The
      // bytes live in the input stream for the reader's lifetime, so a view
      // is safe.
      PackedTerm term(const_cast<char*>(metaPtr));
      fieldNames_.emplace_back(term.data(), term.size());
      metaPtr += term.memorySize();
    }

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
      assert(fid < fieldNames_.size());
      std::string_view fieldName = fieldNames_[fid];
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
  // per-call linear scan over fieldNames_.
  int32_t fieldId(std::string_view fieldName) const {
    return findFieldId(fieldName);
  }

  // Does this resource contain any entries for fieldName?
  bool hasField(std::string_view fieldName) const {
    return findFieldId(fieldName) >= 0;
  }

  // Read only the entries for fieldName in docID.  See readFieldById for
  // the callback signature and semantics; this overload resolves the fid
  // by name on every call.  Use readFieldById in hot loops to avoid the
  // per-call linear scan over fieldNames_.
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
    assert(targetFid >= 0 && (size_t)targetFid < fieldNames_.size());

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
  int32_t maxDoc() const { return maxDoc_; }
  int64_t maxChunkBytes() const { return maxChunkBytes_; }

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

  // Try to open a StoredFieldsReader for the given resource name in the
  // segment (default: Postings::STORED_DEFAULT_RESOURCE).  Returns nullptr
  // if that resource isn't present in the segment.  The FieldReader used
  // for the lookup is transient - no persistent pool allocations.
  static std::unique_ptr<StoredFieldsReader> open(
      PostingsReader& postingsReader,
      std::string_view resourceName = Postings::STORED_DEFAULT_RESOURCE) {
    auto& pool = MemPool::threadLocal();
    auto guard = pool.rewindScopeGuard();
    FieldReader fieldReader(postingsReader);
    if (!fieldReader.seek(resourceName)) {
      return nullptr;
    }
    SegFieldInfo fi;
    fieldReader.readFieldInfo(fi);
    return std::make_unique<StoredFieldsReader>(postingsReader, fi);
  }

private:
  // Linear scan of fieldNames_; typical segments have O(10) stored fields, so
  // a map isn't worth the overhead.
  int32_t findFieldId(std::string_view fieldName) const {
    for (size_t i = 0; i < fieldNames_.size(); i++) {
      if (fieldNames_[i] == fieldName) return (int32_t)i;
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
    int decoded = LZ4_decompress_safe(
        chunkPtr, scratch.data(), compressedSize, uncompressedSize);
    if (decoded != uncompressedSize) {
      throw std::runtime_error("StoredFieldsReader: LZ4 decompression failed");
    }
    cachedChunkNum = chunkNum;
  }
};

}  // namespace solux
