// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <boost/unordered/unordered_flat_map.hpp>
#include <lz4.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "IntColWriter.h"
#include "PostingsWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/StrRef.h"
#include "luxir/util/encoding.h"

namespace luxir {

// Segment-level writer for stored fields
//
// Accumulates raw field values per document into whole-doc chunks, LZ4-compresses
// them (~16KB uncompressed target or 128 docs, whichever comes first), and records
// two monotonic columns so the reader can locate the chunk for any docID:
//   - chunk->firstDocID      (mono, numChunks entries)
//                              Readers find the chunk for a target docID by
//                              binary-searching this column for the largest
//                              chunkNum whose firstDocID <= target.
//   - chunk->fileOffset      (mono2, numChunks+1 entries; last entry is the
//                              end of the chunks region and acts as a sentinel
//                              so the reader can compute every chunk's size)
//
// On-disk chunks file layout:
//   <chunk 0: [int32 uncompressedSize][compressed LZ4 bytes]>
//   <chunk 1: ...>
//   ...
//   <metadata: [vlong maxChunkBytes][varint numFields] then numFields
//              PackedTerms (field names keyed by segment-local stored-field id)>
//
// Uncompressed chunk body (after LZ4 decompress):
//   - an array of numDocs int32 byte offsets, one per doc, giving the doc's
//     start position within the decompressed buffer.  numDocs is not stored
//     here; the reader computes it from the mono column as
//     firstDocCol[chunkNum+1] - firstDocCol[chunkNum] (or
//     maxDoc - firstDocCol[chunkNum] for the last chunk).
//   - then the per-doc content, concatenated.
// Per-doc content:
//   [varint numFieldsInDoc]
//   repeated numFieldsInDoc times:
//     [varint fieldId][varint numValues]
//     repeated numValues times:
//       [varint length][length bytes]
//
// Empty docs (docs with no stored fields) are encoded as a single varint 0 so
// that the per-chunk doc sequence is always contiguous.  This keeps reader
// lookups simple: find chunk by interpolating the first-doc column, then
// numFieldsInDoc=0 means "nothing stored for this doc".
class StoredFieldsWriter {
private:
  PostingsWriter& postingsWriter;
  std::string resourceName_;
  size_t chunkTargetUncompressed_;
  size_t maxDocsPerChunk_;
  OutputStreamPtr chunkOutput;
  int64_t chunksStart = 0;

  // field name -> segment-local stored-field id (transparent lookup avoids
  // materializing a std::string per addValue call).
  boost::unordered_flat_map<std::string, uint32_t,
                            PackedTermHash, PackedTermEqual> fieldNameToId;
  std::vector<PackedTerm> fieldNames;  // indexed by stored-field id

  // Currently-accumulating doc state.
  int32_t currentDoc = -1;
  int32_t currentDocFieldCount = 0;
  std::string currentDocContent;

  // lastFinalizedDoc is the highest docID that has been written into the
  // chunk (or -1 initially).  Used for empty-doc padding.
  int32_t lastFinalizedDoc = -1;

  // Current chunk state.
  std::string chunkBody;  // concatenated doc content (offsets array prepended at flush time)
  std::vector<int32_t> chunkDocOffsets;  // start offset of each doc within chunkBody
  int32_t chunkFirstDoc = -1;

  // Completed chunk metadata.
  std::vector<int64_t> chunkFirstDocs;
  std::vector<int64_t> chunkFileOffsets;  // relative to chunksStart
  int64_t maxChunkBytes = 0;

  // Reusable scratch buffers (avoid per-chunk reallocation).
  std::string uncompressedScratch;
  std::vector<char> compressScratch;

public:
  // Flush trigger defaults.  Overridable per resource via StoredFieldType.
  static constexpr size_t DEFAULT_CHUNK_TARGET = 16 * 1024;
  static constexpr size_t DEFAULT_MAX_DOCS_PER_CHUNK = 128;

  // Create a writer for the named resource.  If config is null, defaults are
  // used.  resourceName is the field name under which the resource is
  // registered in the segment's per-field index (caller holds the schema
  // lookup).  See Postings::STORED_DEFAULT_RESOURCE for the canonical default.
  explicit StoredFieldsWriter(PostingsWriter& pw,
                              std::string_view resourceName = Postings::STORED_DEFAULT_RESOURCE,
                              const StoredFieldType* config = nullptr)
    : postingsWriter(pw),
      resourceName_(resourceName),
      chunkTargetUncompressed_(config ? config->chunkTargetUncompressed_ : DEFAULT_CHUNK_TARGET),
      maxDocsPerChunk_(config ? config->maxDocsPerChunk_ : DEFAULT_MAX_DOCS_PER_CHUNK)
  {
    chunkOutput = postingsWriter.getOutputStream();
    chunksStart = (int64_t)chunkOutput->size();
  }

  std::string_view resourceName() const { return resourceName_; }

  // Append one value for (docID, fieldName).  docID must be non-decreasing
  // across calls; values for the same doc can be emitted across multiple
  // addValue calls (one per field or repeated for multi-valued; use addValues
  // to emit a multi-valued field in a single call).
  void addValue(int32_t docID, std::string_view fieldName, std::string_view value) {
    transitionToDoc(docID);
    uint32_t fid = getOrAssignFieldId(fieldName);
    writeFieldHeader(fid, 1);
    writeValue(value);
  }

  // Append multiple values for (docID, fieldName).  Use for multi-valued fields.
  void addValues(int32_t docID, std::string_view fieldName, std::span<const std::string_view> values) {
    transitionToDoc(docID);
    uint32_t fid = getOrAssignFieldId(fieldName);
    writeFieldHeader(fid, (uint32_t)values.size());
    for (const auto& v : values) {
      writeValue(v);
    }
  }

  void addValues(int32_t docID, std::string_view fieldName, std::span<const std::string* const> values) {
    transitionToDoc(docID);
    uint32_t fid = getOrAssignFieldId(fieldName);
    writeFieldHeader(fid, (uint32_t)values.size());
    for (const auto* v : values) {
      writeValue(*v);
    }
  }

  // Finalize: close the pending doc, pad any skipped docs up to maxDoc with
  // empty entries, flush the last chunk, write metadata and monotonic columns,
  // and register the resource with the PostingsWriter.  No-op if no stored
  // values were added.
  void finish(int32_t maxDoc) {
    // fieldNames.empty() is equivalent to "no addValue call ever reached
    // getOrAssignFieldId" and is the single authoritative check.
    if (fieldNames.empty()) {
      chunkOutput.reset();
      return;
    }
    if (currentDoc >= 0) {
      finalizeCurrentDoc();
    }
    // Pad trailing docs so the chunk sequence covers [0, maxDoc).
    for (int32_t d = lastFinalizedDoc + 1; d < maxDoc; d++) {
      appendEmptyDoc(d);
    }
    if (!chunkDocOffsets.empty()) {
      flushChunk();
    }

    int32_t numChunks = (int32_t)chunkFirstDocs.size();
    assert(numChunks > 0);

    // Chunks region end (offset relative to chunksStart) acts as mono2 sentinel.
    int64_t chunksRegionEnd = (int64_t)chunkOutput->size() - chunksStart;

    // Metadata block follows chunks in the same file.
    chunkOutput->writeVlong((uint64_t)maxChunkBytes);
    chunkOutput->writeVint((uint32_t)fieldNames.size());
    for (const auto& name : fieldNames) {
      chunkOutput->writePackedTerm(name);
    }

    auto& fi = postingsWriter.addField(resourceName_);
    fi.type = FieldType::BIN;
    fi.flags = FieldType::STORED;
    fi.columnLoc = seg_location(chunkOutput->streamNumber, (uint64_t)chunksStart);
    fi.columnMetaOff = chunksRegionEnd;
    fi.numValues = numChunks;
    fi.docsWithField = maxDoc;

    // Drop the chunks output stream before opening new ones for the monotonic columns.
    chunkOutput.reset();

    {
      auto& tmpPool = MemPool::threadLocal();
      auto guard = tmpPool.rewindScopeGuard();
      OutputStreamPtr monoOut = postingsWriter.getOutputStream();
      MonoWriter mono(tmpPool, *monoOut);
      for (int64_t v : chunkFirstDocs) {
        mono.addInt64(v);
      }
      mono.finish();
      fi.monoLoc = mono.blockLoc;
      fi.monoMetaOff = mono.metaOff;
    }

    {
      auto& tmpPool = MemPool::threadLocal();
      auto guard = tmpPool.rewindScopeGuard();
      OutputStreamPtr mono2Out = postingsWriter.getOutputStream();
      MonoWriter mono2(tmpPool, *mono2Out);
      for (int64_t v : chunkFileOffsets) {
        mono2.addInt64(v);
      }
      mono2.addInt64(chunksRegionEnd);  // sentinel
      mono2.finish();
      fi.mono2Loc = mono2.blockLoc;
      fi.mono2MetaOff = mono2.metaOff;
    }
  }

  int32_t numChunksWritten() const {
    return (int32_t)chunkFirstDocs.size();
  }

  // --- Verbatim chunk-copy support (merges) -------------------------------
  //
  // Compressed chunk bodies are position-independent (doc offsets are
  // chunk-relative) but reference the segment-local field-id table, so a
  // source segment's chunks may be appended verbatim only when its table maps
  // identically into this writer's.  A merge of a no-deletions source calls:
  //   1. adoptFieldTable(source names)  - false means re-add doc by doc
  //   2. alignForRawAppend(docBase)     - seal/pad so chunks cover [0, docBase)
  //   3. appendRawChunkRegion(...)      - bulk-copy chunks, rebase directory

  // Adopt the source's field-id table: every existing id must map to the same
  // name; names beyond our table extend it.  Returns false (table unchanged)
  // on any mismatch.
  bool adoptFieldTable(std::span<const std::string_view> names) {
    size_t common = (std::min)(names.size(), fieldNames.size());
    for (size_t i = 0; i < common; i++) {
      if (std::string_view(fieldNames[i].data(), fieldNames[i].size()) != names[i]) {
        return false;
      }
    }
    for (size_t i = fieldNames.size(); i < names.size(); i++) {
      uint32_t id = (uint32_t)fieldNames.size();
      fieldNames.emplace_back(postingsWriter.copyTerm(names[i]));
      fieldNameToId.emplace(std::string(names[i]), id);
    }
    return true;
  }

  // Seal any partially accumulated chunk and pad empty docs so the written
  // chunk sequence covers exactly [0, nextDoc).
  void alignForRawAppend(int32_t nextDoc) {
    if (currentDoc >= 0) {
      finalizeCurrentDoc();
    }
    for (int32_t d = lastFinalizedDoc + 1; d < nextDoc; d++) {
      appendEmptyDoc(d);
    }
    if (!chunkDocOffsets.empty()) {
      flushChunk();
    }
    assert(lastFinalizedDoc == nextDoc - 1);
  }

  // Append a source segment's entire chunks region verbatim.  region/bytes
  // are the source chunks ([int32 uncompressedSize][LZ4 bytes] each, metadata
  // excluded); firstDocs/offsets its chunk directory (offsets relative to the
  // region start); srcMaxDoc its doc count and srcMaxChunkBytes its stored
  // max uncompressed chunk size.
  void appendRawChunkRegion(int32_t docBase, const char* region, int64_t regionBytes,
                            std::span<const int64_t> firstDocs,
                            std::span<const int64_t> offsets,
                            int32_t srcMaxDoc, int64_t srcMaxChunkBytes) {
    assert(currentDoc < 0 && chunkDocOffsets.empty());
    assert(lastFinalizedDoc == docBase - 1);
    assert(firstDocs.size() == offsets.size() && !firstDocs.empty());
    int64_t outBase = (int64_t)chunkOutput->size() - chunksStart;
    for (size_t i = 0; i < firstDocs.size(); i++) {
      chunkFirstDocs.push_back(docBase + firstDocs[i]);
      chunkFileOffsets.push_back(outBase + offsets[i]);
    }
    chunkOutput->write(region, (size_t)regionBytes);
    maxChunkBytes = (std::max)(maxChunkBytes, srcMaxChunkBytes);
    lastFinalizedDoc = docBase + srcMaxDoc - 1;
  }

private:
  void transitionToDoc(int32_t docID) {
    assert(docID >= 0);
    assert(docID >= currentDoc);
    if (docID == currentDoc) return;
    if (currentDoc >= 0) {
      finalizeCurrentDoc();
    }
    // Pad skipped docs (no stored fields) between the previous finalized doc and this one.
    for (int32_t d = lastFinalizedDoc + 1; d < docID; d++) {
      appendEmptyDoc(d);
    }
    currentDoc = docID;
    currentDocFieldCount = 0;
    currentDocContent.clear();
  }

  uint32_t getOrAssignFieldId(std::string_view fieldName) {
    auto it = fieldNameToId.find(fieldName);
    if (it != fieldNameToId.end()) {
      return it->second;
    }
    uint32_t id = (uint32_t)fieldNames.size();
    fieldNames.emplace_back(postingsWriter.copyTerm(fieldName));
    fieldNameToId.emplace(std::string(fieldName), id);
    return id;
  }

  void writeFieldHeader(uint32_t fid, uint32_t numValues) {
    appendVint(currentDocContent, fid);
    appendVint(currentDocContent, numValues);
    currentDocFieldCount++;
  }

  void writeValue(std::string_view value) {
    appendVint(currentDocContent, (uint32_t)value.size());
    currentDocContent.append(value.data(), value.size());
  }

  void finalizeCurrentDoc() {
    assert(currentDoc >= 0);
    if (chunkFirstDoc < 0) {
      chunkFirstDoc = currentDoc;
    }
    chunkDocOffsets.push_back((int32_t)chunkBody.size());
    appendVint(chunkBody, (uint32_t)currentDocFieldCount);
    chunkBody.append(currentDocContent);
    lastFinalizedDoc = currentDoc;
    currentDoc = -1;
    currentDocFieldCount = 0;
    currentDocContent.clear();

    maybeFlushChunk();
  }

  void appendEmptyDoc(int32_t docID) {
    if (chunkFirstDoc < 0) {
      chunkFirstDoc = docID;
    }
    chunkDocOffsets.push_back((int32_t)chunkBody.size());
    appendVint(chunkBody, 0);  // numFields = 0
    lastFinalizedDoc = docID;
    maybeFlushChunk();
  }

  void maybeFlushChunk() {
    size_t uncompressedSize = chunkDocOffsets.size() * sizeof(int32_t) + chunkBody.size();
    if (uncompressedSize >= chunkTargetUncompressed_
        || chunkDocOffsets.size() >= maxDocsPerChunk_) {
      flushChunk();
    }
  }

  void flushChunk() {
    assert(!chunkDocOffsets.empty());
    int32_t numDocs = (int32_t)chunkDocOffsets.size();
    int32_t offsetsArraySize = numDocs * (int32_t)sizeof(int32_t);

    // Assemble uncompressed buffer.  Doc offsets are stored relative to the
    // start of the uncompressed buffer so a reader can index directly.
    uncompressedScratch.clear();
    uncompressedScratch.reserve((size_t)offsetsArraySize + chunkBody.size());
    for (int32_t off : chunkDocOffsets) {
      int32_t adjusted = off + offsetsArraySize;
      uncompressedScratch.append((const char*)&adjusted, sizeof(int32_t));
    }
    uncompressedScratch.append(chunkBody);

    int32_t uncompressedSize = (int32_t)uncompressedScratch.size();
    maxChunkBytes = std::max(maxChunkBytes, (int64_t)uncompressedSize);
    int maxCompressed = LZ4_compressBound(uncompressedSize);
    compressScratch.resize((size_t)maxCompressed);
    int compressedSize = LZ4_compress_default(
        uncompressedScratch.data(), compressScratch.data(),
        uncompressedSize, maxCompressed);
    if (compressedSize <= 0) {
      throw std::runtime_error("StoredFieldsWriter: LZ4 compression failed");
    }

    // Record chunk position before writing (offsets are relative to chunksStart).
    int64_t chunkFileOffset = (int64_t)chunkOutput->size() - chunksStart;
    chunkFirstDocs.push_back(chunkFirstDoc);
    chunkFileOffsets.push_back(chunkFileOffset);

    chunkOutput->writeInt(uncompressedSize);
    chunkOutput->write(compressScratch.data(), (size_t)compressedSize);

    chunkDocOffsets.clear();
    chunkBody.clear();
    chunkFirstDoc = -1;
  }

  static void appendVint(std::string& dest, uint32_t val) {
    while (val > 0x7f) {
      dest.push_back((char)(val | 0x80));
      val >>= 7;
    }
    dest.push_back((char)val);
  }
};

}  // namespace luxir
