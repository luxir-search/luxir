#pragma once

#include "PostingsReader.h"

namespace solux {


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

public:
  IntColReader(MemPool &pool, PostingsReader &postingsReader, const SegFieldInfo &fieldInfo) :
  docs(pool, postingsReader, fieldInfo),
  fieldInfo(fieldInfo)
  {
    columnIS = postingsReader.getInputStreamSeek(fieldInfo.columnLoc);
    blockMeta = reinterpret_cast<const NumericBlockInfo *>(columnIS.ptr(fieldInfo.columnMeta.offset()));
    blocks = reinterpret_cast<const char *>(columnIS.ptr(fieldInfo.columnLoc.offset()));
  }

  int32_t docsWithValue() {
    return fieldInfo.docsWithField;
  }

  template <class DocAcceptor>
  void pushDocs(DocAcceptor docAcceptor) {
    // TODO OPT: push the acceptor right down into screaming bitset! This avoids switching on the bucket type in the bitset!
    // Make a merging benchmark first though!
  }

  // An iterator over dense int values.  It needs to support more than int32 indexes because
  // of multi-valued fields (i.e. even if you only have 2B docs, a column could have > 4B values).
  class DenseIterator {
    const NumericBlockInfo* blockMeta;  // array of block metadata
    const char* blocks;                 // start of the compressed blocks of data
    int64_t index_ = -1;
    int64_t max;
  public:

    DenseIterator(const IntColReader& col) : blockMeta(col.blockMeta), blocks(col.blocks) {
      max = col.fieldInfo.docsWithField;
    }

    int64_t index() {
      return index_;
    }

    int64_t value() {
      return valueAtRank(index_);
    }

    int64_t next() {
      if (++index_ >= max) {
        index_ = ENDINDEX;
      }
      return index_;
    }

    int64_t valueAtRank(int64_t rank) {
      assert (rank >= 0 && rank < max);
      auto blockNum = (uint64_t)rank / Postings::NUMERIC_BLOCK_SIZE;
      auto rankInBlock = (uint64_t)rank % Postings::NUMERIC_BLOCK_SIZE;
      auto& block = blockMeta[blockNum];
      const char* blockStart = blocks + block.blockOffset;
      // depending ont the exact format, valuesInBlock may not be needed.
      auto valuesInBlock = uint32_t(max) % Postings::NUMERIC_BLOCK_SIZE;
      if (block.format <= 32) {
        auto unscaled = Postings::numericCodec.select(blockStart, valuesInBlock, rankInBlock);
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


  // This is an iterator over documents, so indexes will always be 32 bit.
  class Iterator {
    const IntColReader& col;
    screaming::BitSet::Iterator docsIter;
    DenseIterator denseIter;
    int32_t docRank = -1;
    int32_t doc = -1;
    int32_t maxRank;
    bool dense;
  public:

    Iterator(const IntColReader& col) : col(col), docsIter(col.docs.bitset()), denseIter(col) {
      maxRank = col.fieldInfo.docsWithField;
      dense = !col.docs.hasBitset();
    }

    int64_t docId() {
      return doc;
    }

    /*** Don't expose this unless needed ... it could be tough to implement for some encodings!
    int32_t rank() {
      return docRank;
    }
    */

    int64_t value() {
      return denseIter.valueAtRank(docRank);
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

  // class MonotonicReader

};

// Monotonic int col
class MonoColReader {
public:
  // For monotonic fields, we store the slope of the line and interpolate the values (and store
  // the delta from the expected value).
  // Instead of using floating point math when decoding, we use a scaled slope and do integer math.
  // Our integral slope estimate should be within .25 of the expected value calculated with doubles.
  // This leaves the rest of the bits to interpolate large values: 2^64/(16384*4) = 2.8e14
  constexpr static uint64_t MONOTONIC_SLOPE_SCALE = Postings::NUMERIC_BLOCK_SIZE * 4;

  using NumericBlockInfo = IntColReader::NumericBlockInfo;
private:
  InputStream columnIS;
  const SegFieldInfo& fieldInfo;
  const IntColReader::NumericBlockInfo* blockMeta;  // array of block metadata
  const char* blocks;                 // start of the compressed blocks of data


  MonoColReader(MemPool &pool, PostingsReader &postingsReader, const SegFieldInfo &fieldInfo) :
          fieldInfo(fieldInfo)
  {
    columnIS = postingsReader.getInputStreamSeek(fieldInfo.columnLoc);
    blockMeta = reinterpret_cast<const NumericBlockInfo *>(columnIS.ptr(fieldInfo.columnMeta.offset()));
    blocks = reinterpret_cast<const char *>(columnIS.ptr(fieldInfo.columnLoc.offset()));
  }

  int32_t numValues() {
    // The way we currently use MonoColReader, it's always one-for-one with the number of docs with a field value.
    // We could chose to implement sparse values with this sometimes, and then we'd need to return maxDoc.
    return fieldInfo.docsWithField;
  }



};

} // end namespace