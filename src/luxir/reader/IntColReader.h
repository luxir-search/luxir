// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
#include <algorithm>
#include <cstddef>
#include <span>

#include "DocsReader.h"
#include "ReaderErrors.h"
#include "MonoReader.h"
#include "PostingsReader.h"
#include "luxir/codec/NumColumn.h"
#include "luxir/codec/LinearPack.h"
#include "luxir/store/InputStream.h"

namespace luxir {

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

private:
  DocsReader docs;
  NumColumn values;
  const char* zoneMeta = nullptr;
  NumColumnFormat::LeafMeta leaves;
  MonoReader endValueRanks;  // exists if multi-valued.
  bool arrayMode = false;
  void open(const char* blocks, std::span<const char> index, int64_t numValues) {
    values = NumColumn(blocks, index.data(), numValues);
    zoneMeta = index.data() + numBlocks() * sizeof(NumBlockInfo);
  }

public:
  IntColReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : docs(reader, fieldInfo),
        arrayMode((fieldInfo.flags & FieldType::MULTI_ARRAY) != 0) {
    const auto* number = fieldInfo.numberColumn();
    if (!number) missingColumn(fieldInfo, "number");
    auto blocks = reader.core().slice(number->blocks());
    auto indexSlice = number->index();
    auto index = reader.core().slice(indexSlice, ReadClass::METADATA);
    leaves = readNumericLeafMeta(index, fieldInfo.numValues,
        segmentReadContext(reader.core(), fieldInfo, indexSlice.location.filenum()));
    open(blocks.data(), index, fieldInfo.numValues);
    endValueRanks = MonoReader::openEndValueRanks(reader, fieldInfo);
  }

  IntColReader(const char* blocks, std::span<const char> index, int64_t numValues)
      : docs((int32_t)numValues) {
    leaves = readNumericLeafMeta(index, numValues);
    open(blocks, index, numValues);
  }

  const DocsReader& docsReader() const noexcept {
    return docs;
  }

  // Returns the end value ranks for a multi-valued column.
  MonoReader* getEndValueRankReader() {
    return endValueRanks ? &endValueRanks : nullptr;
  }

  // Number of values in the field, including repeats in arrays.
  int64_t numValues() const {
    return values.numValues();
  }

  int32_t docsWithField() const {
    return docs.docsWithField();
  }

  bool multiValued() const {
    return endValueRanks.has_value();
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
  bool leafBounded() const { return !multiValued() && docsReader().allRows(); }

  int32_t valuesInBlock(int64_t blockNum) const {
    return values.valuesInBlock(blockNum);
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
    return endValueRanks.valuesAt(docRank);
  }

  /// Retrieves the start value rank for the given doc rank.
  /// only valid if multiValued() is true
  int64_t getStartValueRank(int32_t docRank) {
    return docRank == 0 ? 0 : endValueRanks.valueAt(docRank - 1);
  }

  bool multiSet() const { return multiValued() && !arrayMode; }

  /// First or last stored value rank. Only valid for multi-valued columns.
  int64_t getEdgeValueRank(int32_t docRank, bool last) {
    return last ? endValueRanks.valueAt(docRank) - 1 : getStartValueRank(docRank);
  }

  // Numeric sort selection in encoded order. Set columns read an edge;
  // arrays scan their source sequence. NaN remains a present encoded value.
  int64_t selectedValue(int32_t docRank, bool maximum, auto& values) {
    assert(multiValued());
    if (multiSet()) return values.valueAt(getEdgeValueRank(docRank, maximum));
    return scanSelectedValue(docRank, maximum, values);
  }

  // Out of line so the array scan does not enlarge the comparators' inlined
  // per-document reads.
  LUXIR_NOINLINE int64_t scanSelectedValue(int32_t docRank, bool maximum, auto& values) {
    auto [start, end] = getStartEndValueRank(docRank);
    return selectedRangeValue(start, end, maximum, values);
  }

  // Select over a contiguous range of value ranks, independent of which
  // documents contributed them. Values remain in their encoded order.
  LUXIR_INLINE int64_t selectedRangeValue(int64_t start, int64_t end, bool maximum, auto& values) {
    assert(start < end);
    int64_t selected = values.valueAt(start);
    for (int64_t rank = start + 1; rank < end; rank++) {
      int64_t next = values.valueAt(rank);
      selected = maximum ? std::max(selected, next) : std::min(selected, next);
    }
    return selected;
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

    DocIterator(const IntColReader& col) : col(col), docsIter(col.docs.bitmapIterator()), valueIter(col) {
      maxRank = col.docsWithField();
      dense = col.docs.allRows();
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
  static void getValues(MemPool& pool, PostingsReader& postingsReader,
                        const DecodedFieldRecord& fieldInfo, std::ranges::input_range auto&& sortedIds, auto&& callback) {
    IntColReader intColReader(postingsReader, fieldInfo);
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
  static void getSingleValues(MemPool& pool, PostingsReader& postingsReader,
                        const DecodedFieldRecord& fieldInfo, std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    IntColReader intColReader(postingsReader, fieldInfo);
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
