// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>

#include "DocsReader.h"
#include "PostingsReader.h"
#include "luxir/codec/NumColumn.h"
#include "luxir/codec/LinearPack.h"
#include "luxir/store/InputStream.h"

namespace luxir {

// Monotonic semantic facade over the shared numeric-column physical layer.
class MonoReader {
public:
  static constexpr uint32_t BLOCK_SIZE = MonoColumn::BLOCK_SIZE;
  static constexpr int64_t ENDINDEX = MonoColumn::ENDINDEX;
  using BlockInfo = NumBlockInfo;

private:
  MonoColumn values;

public:
  MonoReader(PostingsReader& postingsReader, seg_location loc, int64_t metaOff,
             int64_t nValues) {
    InputStream columnIS = postingsReader.getInputStreamSeek(loc);
    const char* blocks = columnIS.ptr();
    values = MonoColumn(blocks, blocks + metaOff, nValues);
  }

  MonoReader(InputStream& columnIS, int64_t loc, int64_t metaOff,
             int64_t nValues) {
    const char* blocks = columnIS.ptr(loc);
    values = MonoColumn(blocks, blocks + metaOff, nValues);
  }

  [[nodiscard]] int64_t numValues() const {
    return values.numValues();
  }

  [[nodiscard]] int64_t valueAt(int64_t index) const {
    return values.valueAt(index);
  }

  // Retrieve values[index-1], values[index]. If index is 0, the first value is
  // defined as zero.
  [[nodiscard]] std::pair<int64_t, int64_t> valuesAt(int64_t index) const {
    return values.valuesAt(index);
  }

  class BulkValues {
    MonoColumn::Bulk values;

  public:
    explicit BulkValues(const MonoReader& column) : values(column.values) {}

    int64_t index() const {
      return values.index();
    }

    int64_t value() const {
      return values.value();
    }

    int64_t next() {
      return values.next();
    }

    int64_t advance(int64_t target) {
      return values.advance(target);
    }

    int64_t valueAt(int64_t index) {
      return values.valueAt(index);
    }
  };
};


// Some logical columns have multiple underlying columns implementing them:
// A sparse multi-valued integer column:
//   1) a dense array of single integer values (usually compressed)
//   2) a monotonic array that gives the start (or end) index into (1) for docs that have a value
//   3) a screaming bitset of docs containing the field
//
class IntColReader {
public:
  static constexpr uint32_t BLOCK_SIZE = NumColumn::BLOCK_SIZE;
  // Sentinel values.  ENDDOC is used when indexing documents, since there are only 2B in a segment.
  constexpr static int32_t ENDDOC = std::numeric_limits<int32_t>::max();

  // ENDINDEX is used when indexing values, since there can be more than 2B in a segment due to multi-valued fields.
  constexpr static int64_t ENDINDEX = std::numeric_limits<int64_t>::max();

  using NumericBlockInfo = NumBlockInfo;

  // Segment-wide extrema trailer contract. Values are in the column's stored
  // int64 representation: raw for INT/DATE, sortable-encoded for FLOAT/DOUBLE.
  // Consumers doing interval math must decode floating endpoints first.
  struct EncodedBounds {
    int64_t min = 0;
    int64_t max = 0;
    bool hasValues = false;
  };


private:
  DocsReader docs;
  InputStream columnIS;
  NumColumn values;
  const char* zoneMeta = nullptr;
  NumColumnFormat::LeafMeta leaves;
  std::optional<MonoReader> endValueRankReader;  // exists if multi-valued.
  int64_t nvals;
  int32_t docsWithField = 0;
  int64_t columnMin = 0;
  int64_t columnMax = 0;

  // Zone metadata follows the block descriptors: block zones, then the leaf
  // metadata (NumColumnFormat::LeafMeta). Returns where the bounds trailer
  // starts.
  const char* locateZoneMeta(const char* blockMeta) {
    zoneMeta = blockMeta + numBlocks() * sizeof(NumBlockInfo);
    const char* trailer = nullptr;
    leaves = NumColumnFormat::LeafMeta(
        zoneMeta + numBlocks() * sizeof(NumBlockZone), nvals, trailer);
    return trailer;
  }

public:
  /// The fieldInfo is only used in the constructor and can be discarded after.
  IntColReader(PostingsReader &postingsReader, const SegFieldInfo &fieldInfo) :
  docs(postingsReader, fieldInfo)
  {
    nvals = fieldInfo.numValues;
    docsWithField = fieldInfo.docsWithField;
    columnIS = postingsReader.getInputStreamSeek(fieldInfo.columnLoc);
    const char* blocks = columnIS.ptr();
    const char* blockMeta = blocks + fieldInfo.columnMetaOff;
    values = NumColumn(blocks, blockMeta, nvals);
    const char* minMaxPtr = locateZoneMeta(blockMeta);

    // Read min/max values that come after the zone arrays
    if (nvals > 0) {
      const char* endPtr = columnIS.ptr(0) + columnIS.size();
      columnMin = InputStream::readVlong(minMaxPtr, endPtr);
      columnMax = InputStream::readVlong(minMaxPtr, endPtr);
    }

    if (fieldInfo.monoLoc.offset() > 0) {
      endValueRankReader.emplace(postingsReader, fieldInfo.monoLoc, fieldInfo.monoMetaOff, fieldInfo.docsWithField);
    }
  }

  IntColReader(InputStream &is, int64_t columnOff, int64_t columnMetaOff) : docs(0), columnIS(is) {
    columnIS.seek(columnOff);
    nvals = 0;  // Initialize to 0 - caller should set this if needed
    docsWithField = 0;
    const char* blocks = columnIS.ptr();
    const char* blockMeta = blocks + columnMetaOff;
    values = NumColumn(blocks, blockMeta, nvals);
    zoneMeta = blockMeta;
  }

  IntColReader(InputStream &is, int64_t columnOff, int64_t columnMetaOff, int64_t numValues) : docs(0), columnIS(is) {
    columnIS.seek(columnOff);
    nvals = numValues;
    docsWithField = static_cast<int32_t>(numValues);
    const char* blocks = columnIS.ptr();
    const char* blockMeta = blocks + columnMetaOff;
    values = NumColumn(blocks, blockMeta, nvals);
    const char* minMaxPtr = locateZoneMeta(blockMeta);

    // Read min/max values that come after the zone arrays
    if (nvals > 0) {
      const char* endPtr = columnIS.ptr(0) + columnIS.size();
      columnMin = InputStream::readVlong(minMaxPtr, endPtr);
      columnMax = InputStream::readVlong(minMaxPtr, endPtr);
    }
  }

  DocsReader& docsReader() {
    return docs;
  }

  //get underlying endValueRankReader, null if not multi-valued. Valid as long as IntColReader is valid.
   MonoReader* getEndValueRankReader() {
    return endValueRankReader ? &(*endValueRankReader) : nullptr;
  }

  // Number of values in field.  For a multi-valued field, this will be greater than docsWithValue
  int64_t numValues() const {
    return nvals;
  }

  int32_t docsWithValue() const {
    return docsWithField;
  }

  bool multiValued() const {
    return endValueRankReader.has_value();
  }
  
  int64_t getMin() const {
    return columnMin;
  }
  
  int64_t getMax() const {
    return columnMax;
  }

  EncodedBounds encodedBounds() const {
    return {columnMin, columnMax, nvals > 0};
  }

  int64_t numBlocks() const {
    return values.numBlocks();
  }

  NumericBlockInfo blockInfo(int64_t blockNum) const {
    return values.blockInfo(blockNum);
  }

  std::optional<NumColumnFormat::RankBounds> rankBounds(
      int64_t blockNum, int64_t lo, int64_t hi) const {
    return values.rankBounds(blockNum, lo, hi);
  }

  NumBlockZone blockZone(int64_t blockNum) const {
    assert(blockNum >= 0 && blockNum < numBlocks());
    NumBlockZone zone;
    memcpy(&zone, zoneMeta + blockNum * sizeof(zone), sizeof(zone));
    return zone;
  }

  // Exact per-LEAF_ZONE_SIZE-value bounds, one level under blockZone().
  // Leaf `l` covers value ranks [l * LEAF_ZONE_SIZE,
  // min((l + 1) * LEAF_ZONE_SIZE, numValues)).
  int64_t numLeafZones() const {
    return leaves.numLeafZones();
  }

  // The leaf zones, the persisted leaf visit orders and the value sketch
  // as one copyable view (NumColumnFormat::LeafMeta).
  const NumColumnFormat::LeafMeta& leafMeta() const { return leaves; }

  // Whether the leaf zones bound docs' keys: a dense single-valued column,
  // where doc == value rank.
  bool leafBounded() const { return !multiValued() && denseDocsWithValue(); }

  int32_t valuesInBlock(int64_t blockNum) const {
    return values.valuesInBlock(blockNum);
  }

  bool denseDocsWithValue() const {
    return !docs.hasBitset();
  }

  const screaming::BitSet& docsWithValueBitSet() const {
    assert(docs.hasBitset());
    return docs.bitset();
  }

  // Decode the 128-value codec sub-block containing valueRank without restoring
  // block min/gcd. The returned residuals are directly comparable with query
  // bounds transformed into the block's residual domain. Returns the global
  // value rank of decoded[0] and writes the number of valid values to count.
  int64_t decodeResidualSubBlock(int64_t valueRank, uint32_t* decoded,
                                 uint32_t& count) const {
    return values.decodeResidualFrame(valueRank, decoded, count);
  }

  // Decode reconstructed values for packed predicted/wide or raw blocks.
  int64_t decodeValueSubBlock(int64_t valueRank, int64_t* decoded,
                            uint32_t& count) const {
    return values.decodeFrame(valueRank, decoded, count);
  }

  /// Retrieves the [startValueRank, endValueRank) range for the given doc rank.
  /// only valid if multiValued() is true
  std::pair<int64_t, int64_t> getStartEndValueRank(int32_t docRank) {
    return endValueRankReader->valuesAt(docRank);
  }

  /// Retrieves the start value rank for the given doc rank.
  /// only valid if multiValued() is true
  int64_t getStartValueRank(int32_t docRank) {
    return docRank == 0 ? 0 : endValueRankReader->valueAt(docRank - 1);
  }

  template <class DocAcceptor>
  void pushDocs(DocAcceptor docAcceptor) {
    // TODO OPT: push the acceptor right down into screaming bitset! This avoids switching on the bucket type in the bitset!
    // Make a merging benchmark first though!
  }

  // Decodes one value per call; tuned for scattered / big-skip (sparse) access.
  // Its sibling BulkValues decodes a sub-block at a time for dense iteration.
  // It needs to support more than int32 indexes because of multi-valued fields
  // (i.e. even if you only have 2B docs, a column could have > 4B values).
  class SparseValues {
    const NumColumn& values;
    int64_t index_ = -1;
    int64_t max;
  public:

    SparseValues(const IntColReader& col)
        : values(col.values), max(col.numValues()) {}

    int64_t index() {
      return index_;
    }

    int64_t value() {
      return valueAt(index_);
    }

    int64_t next() {
      if (++index_ >= max) {
        index_ = ENDINDEX;
      }
      return index_;
    }

    // NOTE: rank must be in bounds!
    int64_t valueAt(int64_t index) {
      return values.valueAt(index);
    }

    int64_t advance(int64_t target) {
      assert (target >= 0 && target < max);
      index_ = target;
      return target;
    }
  };

  /// Decodes sub-blocks at a time when one needs a decent percent of the values.
  class BulkValues {
    NumColumn::Bulk values;
  public:

    BulkValues(const IntColReader& col) : values(col.values) {}

    int64_t index() {
      return values.index();
    }

    int64_t value() {
      return values.value();
    }

    int64_t next() {
      return values.next();
    }

    int64_t advance(int64_t target) {
      return values.advance(target);
    }

    LUXIR_INLINE int64_t valueAt(int64_t index) {
      return values.valueAt(index);
    }
  };


  // This is an iterator over documents, so indexes will always be 32 bit.
  template <class ValuesImpl>
  class DocIterator {
  protected:
    const IntColReader& col;
    screaming::BitSet::Iterator docsIter;
    ValuesImpl valueIter;
    int32_t docRank = -1;
    int32_t doc = -1;
    int32_t maxRank;
    bool dense;
  public:

    DocIterator(const IntColReader& col) : col(col), docsIter(col.docs.bitset()), valueIter(col) {
      maxRank = col.docsWithValue();
      dense = !col.docs.hasBitset();
    }

    [[gnu::cold]] void reset() {
      docsIter.reset();
      docRank = doc = -1;
      // Value blocks are keyed by ordinal and remain valid across rewinds.
    }

    ValuesImpl& values() {
      return valueIter;
    }

    int64_t docId() {
      return doc;
    }

    int32_t rank() {
      return docRank;
    }

    /// for single-valued fields only!  For multi-valued
    /// use values() to get the start and end ranks.
    LUXIR_INLINE int64_t value() {
      return valueIter.valueAt(docRank);
    }

    // TODO: directly expose docsIter (or the screaming set) here to enable bulk / direct operations on them?

    // Always inlined: the facet and stats column walks call this once per
    // document, and its dense branch is a few instructions. Leaving it to the
    // inliner made it an out-of-line call whenever another caller appeared
    // elsewhere in the translation unit (int facets ran 40-67% slower).
    LUXIR_INLINE int32_t advance(int32_t target) {
      if (dense) {
        if (target >= maxRank) {
          doc = ENDDOC;
        } else {
          doc = docRank = target;
        }
      } else {
        doc = docsIter.advance(target);
        docRank = docsIter.rank();
      }
      return doc;
    }

    // returns the docid corresponding to the next value
    LUXIR_INLINE int32_t next() {
      if (docRank + 1 >= maxRank) {
        doc = ENDDOC;
        return doc;
      }
      docRank++;

      if (dense) {
        doc++;
      } else {
        doc = docsIter.next();
      }

      return doc;
    }

    // Steps to the first later document for which accept(doc, rank) holds,
    // or ENDDOC, as repeated next() calls would. The cursor stays in locals
    // and is stored once on return: a scan that stores it for every document
    // pays a store/reload chain whose cost depended on where the iterator
    // landed in memory (forced column full scans varied by 15%).
    template <typename Accept>
    LUXIR_INLINE int32_t nextWhere(Accept&& accept) {
      int32_t rank = docRank;
      int32_t end = maxRank;
      if (dense) {
        while (++rank < end) {
          if (accept(rank, rank)) {
            docRank = rank;
            return doc = rank;
          }
        }
      } else {
        while (++rank < end) {
          int32_t next = docsIter.next();
          if (accept(next, rank)) {
            docRank = rank;
            return doc = next;
          }
        }
      }
      docRank = rank - 1;
      return doc = ENDDOC;
    }
  };

  using SparseIterator = DocIterator<SparseValues>;  // decodes individual values (good for big skipping)
  using BulkIterator = DocIterator<BulkValues>;  // decodes blocks of values (good for iterating or small skipping)
  using Iterator = BulkIterator;

  /// docids is a sorted range of docids to load values for in a multi-valued field.
  /// calls
  ///    callback(size_t input_index, int32_t docid, int64_t value, int64_t value_index, int64_t numValues)
  /// for each docid that has any values.
  /// For example, if docids[4]==1000, and docid 1000 has values {10, 11, 12}, then the callbacks would be:
  /// callback(4, 1000, 10, 0, 3), callback(4, 1000, 11, 1, 3), callback(4, 1000, 12, 2, 3).
  static void getValues(MemPool& pool, PostingsReader& postingsReader, const SegFieldInfo& segFieldInfo, std::ranges::input_range auto&& sortedIds, auto&& callback) {
    IntColReader intColReader(postingsReader, segFieldInfo);
    IntColReader::Iterator iter(intColReader);

    int32_t foundid = -1;
    size_t idx = 0;
    for (int32_t docid : sortedIds) {
      if (foundid < docid) {
        foundid = iter.advance(docid);
      }
      if (foundid == docid) {
        if (!intColReader.multiValued()) {
          auto val = iter.value();
          callback(idx, docid, val, 0, 1);
        } else {
          auto [start, end] = intColReader.getStartEndValueRank(iter.rank());
          auto n = end - start;
          for (int64_t vrank = 0; vrank < n; vrank++) {
            auto val = iter.values().valueAt(start + vrank);
            callback(idx, docid, val, vrank, n);
          }
        }
      } else if (foundid == IntColReader::ENDDOC) {
        break;
      }
      idx++;
    }
  }

  /// docids is a sorted range of docids to load values for in a multi-valued field.
  /// calls callback(size_t input_index, int32_t docid, int64_t value) for each docid that has any values.
  static void getSingleValues(MemPool& pool, PostingsReader& postingsReader, const SegFieldInfo& segFieldInfo, std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    IntColReader intColReader(postingsReader, segFieldInfo);
    IntColReader::Iterator iter(intColReader);
    assert(!intColReader.multiValued());
    int32_t foundid = -1;
    size_t idx = 0;
    for (int32_t docid : sortedDocIds) {
      if (foundid < docid) {
        foundid = iter.advance(docid);
      }
      if (foundid == docid) {
        auto v = iter.value();
        callback(idx, docid, v);
      } else if (foundid == IntColReader::ENDDOC) {
        break;
      }
      idx++;
    }
  }


};


} // end namespace
