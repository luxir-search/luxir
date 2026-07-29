#pragma once

#include <cstddef>

#include "DocsReader.h"
#include "PostingsReader.h"
#include "solux/codec/NumColumnFormat.h"
#include "solux/codec/LinearPack.h"
#include "solux/store/InputStream.h"

namespace solux {

// Shared numeric-column physical decode layer. Descriptors are read in place.
//
// The block-meta array must be 8-aligned - NumBlockInfo is a naturally aligned
// struct read straight off the mapping. NumColumnWriter::finish aligns it in
// the stream, so anything that RELOCATES a finished column has to preserve
// that; OrdMap appends columns into one buffer and pads to 8 (OrdMapImpl.h).
// The constructors assert it so a new relocator fails loudly rather than
// silently reading misaligned descriptors.
//
// WithGcd is a compile-time property of the column, not of a block: monotonic
// columns are written with gcd pinned to 1 (the slope carries any regular
// step), so their decode drops the field load and the multiply entirely rather
// than branching around them.
template <bool WithGcd>
class NumColumnT {
public:
  static constexpr uint32_t BLOCK_SIZE = NumColumnFormat::BLOCK_SIZE;
  static constexpr uint32_t BULK_SIZE = NumColumnFormat::BULK_SIZE;
  static constexpr int64_t ENDINDEX = std::numeric_limits<int64_t>::max();

private:
  const char* blocks = nullptr;
  const char* blockMeta = nullptr;
  int64_t nValues = 0;

  static int64_t reconstruct(const NumBlockInfo& info, uint64_t rankInBlock,
                             uint64_t residual) {
    // Constant blocks are the common case for unordered numeric data, and
    // slopeTerm costs two multiplies that produce zero for them. The test is
    // per-block-shaped so it predicts; decodeFrame splits the same way.
    uint64_t scaled = residual;
    if (info.scaledSlope != 0) {
      scaled += (uint64_t)NumColumnFormat::slopeTerm(rankInBlock,
                                                     info.scaledSlope);
    }
    if constexpr (WithGcd) {
      return (int64_t)(info.baseBits + info.gcd * scaled);
    } else {
      assert(info.gcd == 1);
      return (int64_t)(info.baseBits + scaled);
    }
  }

public:
  NumColumnT() = default;

  NumColumnT(const char* blocks, const char* blockMeta, int64_t nValues)
      : blocks(blocks), blockMeta(blockMeta), nValues(nValues) {
    // Descriptors are read in place, so a relocated column that lost its
    // 8-alignment lands here rather than in a misaligned load downstream.
    assert(((uintptr_t)blockMeta & (alignof(NumBlockInfo) - 1)) == 0);
  }

  int64_t numValues() const {
    return nValues;
  }

  int64_t numBlocks() const {
    return (nValues + BLOCK_SIZE - 1) / BLOCK_SIZE;
  }

  // Read in place. NumBlockInfo is unaligned, so field access compiles to the
  // same loads a copy would make, without materializing the struct.
  const NumBlockInfo& blockInfo(int64_t blockNum) const {
    assert(blockNum >= 0 && blockNum < numBlocks());
    return ((const NumBlockInfo*)blockMeta)[blockNum];
  }

  int32_t valuesInBlock(int64_t blockNum) const {
    assert(blockNum >= 0 && blockNum < numBlocks());
    int64_t start = blockNum * (int64_t)BLOCK_SIZE;
    return (int32_t)std::min<int64_t>(BLOCK_SIZE, nValues - start);
  }

  int64_t valueAt(int64_t rank) const {
    assert(rank >= 0 && rank < nValues);
    uint64_t rankInBlock = (uint64_t)rank % BLOCK_SIZE;
    const NumBlockInfo& info =
        blockInfo((int64_t)((uint64_t)rank / BLOCK_SIZE));
    const char* payload = blocks + info.payloadOffset();
    if (info.bits() > NumColumnFormat::MAX_PACKED_BITS) {
      return loadUnaligned<int64_t>(payload + rankInBlock * sizeof(int64_t));
    }
    uint64_t residual = LinearPack::select64(
        payload, rankInBlock, info.bits(), LinearPack::mask64(info.bits()));
    return reconstruct(info, rankInBlock, residual);
  }

  // Values at rank-1 and rank, with rank-1 defined as 0 at rank 0. This is how
  // monotonic columns are actually read - a doc's value range is the adjacent
  // pair - and the two ranks share a block except at a boundary, so resolve the
  // descriptor once instead of paying for it twice.
  std::pair<int64_t, int64_t> valuesAt(int64_t rank) const {
    assert(rank >= 0 && rank < nValues);
    // Both cases where the pair does not share a descriptor: rank 0, which has
    // no predecessor, and a block boundary, where rank - 1 is the previous
    // block's last value. rank 0 has rankInBlock 0, so one test covers both.
    // Only rank 0 yields 0; a boundary reads each side normally.
    uint64_t rankInBlock = (uint64_t)rank % BLOCK_SIZE;
    if (rankInBlock == 0) {
      return {rank > 0 ? valueAt(rank - 1) : 0, valueAt(rank)};
    }
    const NumBlockInfo& info =
        blockInfo((int64_t)((uint64_t)rank / BLOCK_SIZE));
    const char* payload = blocks + info.payloadOffset();
    if (info.bits() > NumColumnFormat::MAX_PACKED_BITS) {
      int64_t pair[2];
      memcpy(pair, payload + (rankInBlock - 1) * sizeof(int64_t), sizeof(pair));
      return {pair[0], pair[1]};
    }
    uint64_t mask = LinearPack::mask64(info.bits());
    uint64_t prev = LinearPack::select64(payload, rankInBlock - 1, info.bits(), mask);
    uint64_t cur = LinearPack::select64(payload, rankInBlock, info.bits(), mask);
    return {reconstruct(info, rankInBlock - 1, prev),
            reconstruct(info, rankInBlock, cur)};
  }

  // Decode the 128-aligned frame containing rank. Returns the global rank of
  // out[0] and stores the number of valid values in count.
  //
  // Deliberately out of line. This runs once per 128 values, but it carries
  // every format variant (raw, packed-narrow with three scale-loop shapes,
  // packed-wide), so inlining it into a caller's per-value loop bloats that
  // loop and spills its cursor state to the stack. Measured on dense numeric
  // faceting: inlined, the per-value path reloads decodedStart/decodedEnd from
  // stack every iteration and the whole facet is ~25% slower.
  SOLUX_NOINLINE int64_t decodeFrame(int64_t rank, int64_t* out,
                                     uint32_t& count) const {
    assert(rank >= 0 && rank < nValues);
    int64_t start = rank / BULK_SIZE * BULK_SIZE;
    int64_t blockNum = start / BLOCK_SIZE;
    uint64_t rankInBlock = (uint64_t)start % BLOCK_SIZE;
    const NumBlockInfo& info = blockInfo(blockNum);
    count = (uint32_t)std::min<int64_t>(BULK_SIZE, nValues - start);
    const char* payload = blocks + info.payloadOffset();

    if (info.bits() > NumColumnFormat::MAX_PACKED_BITS) {
      memcpy(out, payload + rankInBlock * sizeof(int64_t),
             count * sizeof(int64_t));
      return start;
    }

    if (info.bits() <= 32) {
      uint32_t residuals[BULK_SIZE];
      LinearPack::unpack128(payload, rankInBlock, count, info.bits(),
                            LinearPack::mask32(info.bits()), residuals);
      if (info.gcd == 1 && info.scaledSlope == 0) {
        for (uint32_t i = 0; i < count; i++) {
          out[i] = (int64_t)(info.baseBits + residuals[i]);
        }
      } else if (info.scaledSlope == 0) {
        for (uint32_t i = 0; i < count; i++) {
          out[i] = (int64_t)(info.baseBits +
              info.gcd * (uint64_t)residuals[i]);
        }
      } else {
        for (uint32_t i = 0; i < count; i++) {
          out[i] = reconstruct(info, rankInBlock + i, residuals[i]);
        }
      }
      return start;
    }

    uint64_t mask = LinearPack::mask64(info.bits());
    for (uint32_t i = 0; i < count; i++) {
      uint64_t residual = LinearPack::select64(
          payload, rankInBlock + i, info.bits(), mask);
      out[i] = reconstruct(info, rankInBlock + i, residual);
    }
    return start;
  }

  class Bulk {
    const NumColumnT& column;
    int64_t index_ = -1;
    int64_t decodedStart = -1;
    int64_t decodedEnd = -1;
    int64_t decoded[BULK_SIZE];

    void decode(int64_t index) {
      uint32_t count;
      decodedStart = column.decodeFrame(index, decoded, count);
      decodedEnd = decodedStart + count;
    }

  public:
    explicit Bulk(const NumColumnT& column) : column(column) {}

    int64_t index() const {
      return index_;
    }

    int64_t value() const {
      assert(index_ >= decodedStart && index_ < decodedEnd);
      return decoded[index_ - decodedStart];
    }

    int64_t next() {
      if (++index_ >= column.numValues()) {
        index_ = ENDINDEX;
        return index_;
      }
      if (index_ >= decodedEnd) decode(index_);
      return index_;
    }

    int64_t advance(int64_t target) {
      assert(target > index_);
      index_ = target;
      if (index_ >= column.numValues()) {
        index_ = ENDINDEX;
        return index_;
      }
      if (index_ < decodedStart || index_ >= decodedEnd) decode(index_);
      return index_;
    }

    int64_t valueAt(int64_t index) {
      assert(index >= 0 && index < column.numValues());
      if (index < decodedStart || index >= decodedEnd) decode(index);
      return decoded[index - decodedStart];
    }
  };
};

// General numeric columns: values are arbitrary, so a per-block common divisor
// is worth finding (coarse-granularity dates are the shape that pays for it).
using NumColumn = NumColumnT<true>;
// Monotonic columns: written with gcd pinned to 1, so the decode drops it.
using MonoColumn = NumColumnT<false>;

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
  std::optional<MonoReader> endValueRankReader;  // exists if multi-valued.
  int64_t nvals;
  int32_t docsWithField = 0;
  int64_t columnMin = 0;
  int64_t columnMax = 0;

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
    zoneMeta = blockMeta + numBlocks() * sizeof(NumBlockInfo);
    
    // Read min/max values that come after the block metadata
    if (nvals > 0) {
      const char* minMaxPtr = zoneMeta + numBlocks() * sizeof(NumBlockZone);
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
    zoneMeta = blockMeta + numBlocks() * sizeof(NumBlockInfo);
    
    // Read min/max values that come after the block metadata
    if (nvals > 0) {
      const char* minMaxPtr = zoneMeta + numBlocks() * sizeof(NumBlockZone);
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

  NumBlockZone blockZone(int64_t blockNum) const {
    assert(blockNum >= 0 && blockNum < numBlocks());
    NumBlockZone zone;
    memcpy(&zone, zoneMeta + blockNum * sizeof(zone), sizeof(zone));
    return zone;
  }

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
    constexpr int64_t SUB_BLOCK_SIZE = 128;
    static_assert(BLOCK_SIZE % SUB_BLOCK_SIZE == 0);
    assert(valueRank >= 0 && valueRank < nvals);
    int64_t start = valueRank / SUB_BLOCK_SIZE * SUB_BLOCK_SIZE;
    int64_t blockNum = start / BLOCK_SIZE;
    int64_t rankInBlock = start % BLOCK_SIZE;
    const NumericBlockInfo& block = blockInfo(blockNum);
    assert(block.bits() <= 32 && block.scaledSlope == 0);
    count = (uint32_t)std::min<int64_t>(SUB_BLOCK_SIZE, nvals - start);
    LinearPack::unpack128(
        columnIS.ptr() + block.payloadOffset(), rankInBlock, count,
        block.bits(), LinearPack::mask32(block.bits()), decoded);
    return start;
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

    int64_t valueAt(int64_t index) {
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
    int64_t value() {
      return valueIter.valueAt(docRank);
    }

    // TODO: directly expose docsIter (or the screaming set) here to enable bulk / direct operations on them?

    int32_t advance(int32_t target) {
      if (dense) {
        if (target >= maxRank) {
          doc = ENDDOC;
        } else {
          doc = docRank = target;
        }
      } else { // Would a sparse-only iterator (so we don't have the dense code in here) improve performance?
        doc = docsIter.advance(target);
        docRank = docsIter.rank();
      }
      return doc;
    }

    // returns the docid corresponding to the next value
    int32_t next() {
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
  static void getValues(MemPool& pool, PostingsReader& postingsReader, SegFieldInfo& segFieldInfo, std::ranges::input_range auto&& sortedIds, auto&& callback) {
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
  static void getSingleValues(MemPool& pool, PostingsReader& postingsReader, SegFieldInfo& segFieldInfo, std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
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

  /// see getSingleValues(), but starting with a field name.
  static void getSingleValues(MemPool& pool, PostingsReader& postingsReader, std::string_view field, std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    FieldReader fieldReader(postingsReader);
    bool found = fieldReader.seek(field);
    if (!found) {
      return;
    }
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);

    getSingleValues(pool, postingsReader, segFieldInfo, sortedDocIds, callback);
  }

};


} // end namespace
