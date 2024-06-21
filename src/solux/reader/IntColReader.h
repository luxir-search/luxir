#pragma once

#include "PostingsReader.h"

namespace solux {



// Monotonic int col.  Currently supports 32 bit indexes and 64 bit outputs.
class MonoReader {
public:
  constexpr static uint32_t BLOCK_SIZE = Postings::NUMERIC_BLOCK_SIZE;
  // For monotonic fields, we store the slope of the line and interpolate the values (and store
  // the delta from the expected value).
  // Instead of using floating point math when decoding, we use a scaled slope and do integer math.
  // Our integral slope estimate should be within .25 of the expected value calculated with doubles.
  // This leaves the rest of the bits to interpolate large values: 2^64/(16384*4) = 2.8e14
  constexpr static uint64_t SLOPE_SCALE = BLOCK_SIZE * 4;

  // either make this struct packed, or round it out so there won't be any undefined padding between elements.
  SOLUX_PACKED_START
  struct BlockInfo {
    uint64_t blockOffset;
    uint64_t scaledSlope;
    int32_t intercept;
    uint8_t bits;
    uint8_t _padding[3]={0,0,0};
  } SOLUX_PACKED_END;

protected:
  InputStream columnIS;
  const BlockInfo* blockMeta;  // array of block metadata
  const char* blocks;          // start of the compressed blocks of data
  int32_t nValues;

public:
  MonoReader(MemPool &pool, PostingsReader &postingsReader, seg_location loc, int64_t metaOff, int32_t nValues)
  {
    columnIS = postingsReader.getInputStreamSeek(loc);
    blocks = columnIS.ptr();
    blockMeta = reinterpret_cast<const BlockInfo *>(blocks + metaOff);
    this->nValues = nValues;
  }

  MonoReader(MemPool &pool, InputStream is, int64_t loc, int64_t metaOff, int32_t nValues)
  {
    columnIS = is;
    blocks = columnIS.ptr(loc);
    blockMeta = reinterpret_cast<const BlockInfo *>(blocks + metaOff);
    this->nValues = nValues;
  }

  [[nodiscard]] int32_t numValues() const {
    return nValues;
  }

  [[nodiscard]] int64_t valueAt(int32_t index) const {
    assert (index >= 0 && index < nValues);
    auto blockNum = (uint64_t)index / BLOCK_SIZE;
    auto rankInBlock = (uint64_t)index % BLOCK_SIZE;
    auto& block = blockMeta[blockNum];
    const char* blockStart = blocks + block.blockOffset;
    if (block.bits > 32) {
      return reinterpret_cast<const int64_t*>(blockStart)[rankInBlock];
    }
    // depending on the exact format, valuesInBlock may not be needed.
    auto valuesInBlock = (blockNum == uint64_t(nValues) / BLOCK_SIZE) ? uint32_t(nValues) % BLOCK_SIZE : BLOCK_SIZE;
    auto delta = Postings::numericCodec.selectWithMeta(blockStart, valuesInBlock, rankInBlock, 0, block.bits);
    int64_t scaled = uint64_t(rankInBlock * block.scaledSlope) / SLOPE_SCALE + block.intercept + delta;
    return scaled;
  }

  // Retrieve values[index-1], values[index].  If index is 0, the first value is 0.
  [[nodiscard]] std::pair<int64_t, int64_t> valuesAt(int32_t index) const {
    auto v1 = index > 0 ? valueAt(index - 1) : 0;
    auto v2 = valueAt(index);
    return {v1, v2};
  }

};


// Some logical columns have multiple underlying columns implementing them:
// A sparse multi-valued integer column:
//   1) a dense array of single integer values (usually compressed)
//   2) a monotonic array that gives the start (or end) index into (1) for docs that have a value
//   3) a screaming bitset of docs containing the field
//
class IntColReader {
public:
  // Sentinel values.  ENDDOC is used when indexing documents, since there are only 2B in a segment.
  constexpr static int32_t ENDDOC = std::numeric_limits<int32_t>::max();

  // ENDINDEX is used when indexing values, since there can be more than 2B in a segment due to multi-valued fields.
  constexpr static int64_t ENDINDEX = std::numeric_limits<int64_t>::max();

  struct NumericBlockInfo {
    int64_t gcd;
    int64_t min;
    int64_t max;
    int64_t format; // currently number of bits if <= 32.
    int64_t blockOffset;  // byte offset of compressed block from the start of the column
  };


private:
  DocsReader docs;
  InputStream columnIS;
  const SegFieldInfo &fieldInfo;
  const NumericBlockInfo* blockMeta;  // array of block metadata
  const char* blocks;                 // start of the compressed blocks of data
  MonoReader* endRankReader;          // optional, exists if multi-valued.

public:
  IntColReader(MemPool &pool, PostingsReader &postingsReader, const SegFieldInfo &fieldInfo) :
  docs(pool, postingsReader, fieldInfo),
  fieldInfo(fieldInfo)
  {
    columnIS = postingsReader.getInputStreamSeek(fieldInfo.columnLoc);
    blocks = reinterpret_cast<const char *>(columnIS.ptr());
    blockMeta = reinterpret_cast<const NumericBlockInfo *>(blocks + fieldInfo.columnMetaOff);
    if (fieldInfo.monoLoc.offset() > 0) {
      endRankReader = pool.make<MonoReader>(pool, postingsReader, fieldInfo.monoLoc, fieldInfo.monoMetaOff, fieldInfo.docsWithField);
    } else {
      endRankReader = nullptr;
    }
  }

  // Number of values in field.  For a multi-valued field, this will be greater than docsWithValue
  int64_t numValues() {
    return fieldInfo.numValues;
  }

  int32_t docsWithValue() {
    return fieldInfo.docsWithField;
  }

  bool multiValued() {
    return endRankReader != nullptr;
  }

  /// Retrieves the start and end ranks into the values for the given rank.
  /// only valid if multiValued() is true
  std::pair<int64_t, int64_t> getStartEndRank(int32_t index) {
    return endRankReader->valuesAt(index);
  }

  /// Retrieves the start rank into the values for the given rank.
  /// only valid if multiValued() is true
  int64_t getStartRank(int32_t index) {
    return index == 0 ? 0 : endRankReader->valueAt(index-1);
  }

  template <class DocAcceptor>
  void pushDocs(DocAcceptor docAcceptor) {
    // TODO OPT: push the acceptor right down into screaming bitset! This avoids switching on the bucket type in the bitset!
    // Make a merging benchmark first though!
  }

  // An iterator over dense int values.  It needs to support more than int32 indexes because
  // of multi-valued fields (i.e. even if you only have 2B docs, a column could have > 4B values).
  class DenseValues {
    const NumericBlockInfo* blockMeta;  // array of block metadata
    const char* blocks;                 // start of the compressed blocks of data
    int64_t index_ = -1;
    int64_t max;
  public:

    DenseValues(const IntColReader& col) : blockMeta(col.blockMeta), blocks(col.blocks) {
      max = col.fieldInfo.numValues;
    }

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
      assert (index >= 0 && index < max);
      auto blockNum = (uint64_t)index / Postings::NUMERIC_BLOCK_SIZE;
      auto rankInBlock = (uint64_t)index % Postings::NUMERIC_BLOCK_SIZE;
      auto& block = blockMeta[blockNum];
      const char* blockStart = blocks + block.blockOffset;
      // depending ont the exact format, valuesInBlock may not be needed.
      auto valuesInBlock = (blockNum == uint64_t(max) / Postings::NUMERIC_BLOCK_SIZE) ? uint32_t(max) % Postings::NUMERIC_BLOCK_SIZE : Postings::NUMERIC_BLOCK_SIZE;
      if (block.format <= 32) {
        auto unscaled = Postings::numericCodec.selectWithMeta(blockStart, valuesInBlock, rankInBlock, 0, block.format);
        return unscaled * block.gcd + block.min;
      } else {
        // 64-bit, temp impl uncompressed
        return reinterpret_cast<const int64_t*>(blockStart)[rankInBlock];
      }
    }

    int64_t advance(int64_t target) {
      assert (target >= 0 && target < max);
      index_ = target;
      return target;
    }
  };

  /// Decodes sub-blocks at a time when one needs a decent percent of the values.
  class BulkValues {
    constexpr static uint32_t BULK_DECODE = 128;
    const NumericBlockInfo* blockMeta;  // array of block metadata
    const char* blocks;                 // start of the compressed blocks of data
    int64_t index_ = -1;
    int64_t max;
    int64_t decodedStart = -1;
    int64_t decodedMax = 0;
    // OPT: when max < BULK_DECODE, we don't need this much space.  We could pool allocate if we need to save more memory.
    int64_t decoded[BULK_DECODE];
  public:

    BulkValues(const IntColReader& col) : blockMeta(col.blockMeta), blocks(col.blocks) {
      max = col.fieldInfo.numValues;
    }

    int64_t index() {
      return index_;
    }

    int64_t value() {
      assert (index_ >= decodedStart && index_ < decodedMax);
      return decoded[index_ - decodedStart];
    }

    int64_t next() {
      if (++index_ < decodedMax) {
        return index_;
      }
      if (index_ >= max) {
        index_ = ENDINDEX;
      }
      decodeBlock(index_);
      return index_;
    }

    void decodeBlock(int64_t index) {
      // it's not clear to me what type of alignment will work for SoluxSIMDFor here.
      // For now, we'll be conservative and align to 128.
      assert (index >= 0 && index < max);
      uint64_t start = uint64_t(index) / BULK_DECODE * BULK_DECODE;
      assert(index < start + BULK_DECODE);
      auto bigBlock = start / Postings::NUMERIC_BLOCK_SIZE;
      auto littleBlock = start % Postings::NUMERIC_BLOCK_SIZE / BULK_DECODE;
      auto& block = blockMeta[bigBlock];
      uint32_t littleBlockSize = BULK_DECODE * block.format / 8;
      const char* subBlockStart = blocks + block.blockOffset + littleBlock * littleBlockSize;
      decodedStart = start;
      decodedMax = std::min(decodedStart + BULK_DECODE, max);
      uint32_t num = decodedMax - decodedStart;
      if (block.format <= 32) {
        uint32_t ints[BULK_DECODE];
        Postings::numericCodec.decodeWithMeta(subBlockStart, littleBlockSize,
                                              ints, num, 0, block.format);
        for (uint32_t i = 0; i < num; i++) {
          decoded[i] = ints[i] * block.gcd + block.min;
        }
      } else {
        // 64-bit, temp impl uncompressed
        auto rankInBlock = (uint64_t)index % Postings::NUMERIC_BLOCK_SIZE;
        memcpy(decoded, reinterpret_cast<const int64_t*>(blocks + block.blockOffset) + rankInBlock, num * sizeof(int64_t));
      }
    }

    int64_t advance(int64_t target) {
      assert (target >= 0 && target < max);
      index_ = target;
      return target;
    }

    int64_t valueAt(int64_t index) {
      if (index >= decodedMax || index < decodedStart) {
        decodeBlock(index);
      }
      return decoded[index - decodedStart];
    }

  };


  // This is an iterator over documents, so indexes will always be 32 bit.
  template <class DenseValueImpl>
  class DocIterator {
  protected:
    const IntColReader& col;
    screaming::BitSet::Iterator docsIter;
    DenseValueImpl valueIter;
    int32_t docRank = -1;
    int32_t doc = -1;
    int32_t maxRank;
    bool dense;
  public:

    DocIterator(const IntColReader& col) : col(col), docsIter(col.docs.bitset()), valueIter(col) {
      maxRank = col.fieldInfo.docsWithField;
      dense = !col.docs.hasBitset();
    }

    DenseValueImpl& values() {
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

  using SparseIterator = DocIterator<DenseValues>;  // decodes individual values (good for big skipping)
  using BulkIterator = DocIterator<BulkValues>;  // decodes blocks of values (good for iterating or small skipping)
  using Iterator = BulkIterator;

  /// docids is a sorted range of docids to load values for in a multi-valued field.
  /// calls
  ///    callback(size_t input_index, int32_t docid, int64_t value, int64_t value_index, int64_t numValues)
  /// for each docid that has any values.
  /// For example, if docids[4]==1000, and docid 1000 has values {10, 11, 12}, then the callbacks would be:
  /// callback(4, 1000, 10, 0, 3), callback(4, 1000, 11, 1, 3), callback(4, 1000, 12, 2, 3).
  static void getValues(MemPool& pool, PostingsReader& postingsReader, SegFieldInfo& segFieldInfo, std::ranges::input_range auto&& sortedIds, auto&& callback) {
    IntColReader intColReader(pool, postingsReader, segFieldInfo);
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
          auto [start, end] = intColReader.getStartEndRank(iter.rank());
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
    IntColReader intColReader(pool, postingsReader, segFieldInfo);
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
    FieldReader fieldReader(pool, postingsReader);
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