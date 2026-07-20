#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

#include "DocsReader.h"
#include "IntColReader.h"
#include "PostingsReader.h"
#include "solux/codec/LinearPack.h"
#include "solux/codec/OrdColumnFormat.h"

namespace solux {

class OrdColReader {
public:
  static constexpr uint32_t BLOCK_SIZE = OrdColumnFormat::BLOCK_SIZE;
  static constexpr uint32_t BULK_SIZE = OrdColumnFormat::BULK_SIZE;
  using PredictedBlockInfo = OrdColumnFormat::PredictedBlockInfo;
  static constexpr int32_t ENDDOC = std::numeric_limits<int32_t>::max();
  static constexpr int64_t ENDINDEX = std::numeric_limits<int64_t>::max();

private:
  DocsReader docs;
  InputStream columnIS;
  const char* blocks = nullptr;
  const char* blockMeta = nullptr;
  std::optional<MonoReader> endValueRanks;
  int64_t nvalues = 0;
  int32_t maxdoc = 0;
  int32_t format = SegFieldInfo::ORD_NONE;
  int32_t indexing = SegFieldInfo::ORD_INDEX_NONE;
  uint8_t bits = 0;
  uint32_t mask = 0;
  bool multi = false;

  PredictedBlockInfo blockInfo(uint64_t block) const {
    PredictedBlockInfo info;
    memcpy(&info, blockMeta + block * sizeof(PredictedBlockInfo), sizeof(info));
    return info;
  }

  uint32_t valueAt(int64_t index) const {
    assert(index >= 0 && index < encodedValues());
    if (format == SegFieldInfo::ORD_DIRECT) {
      return LinearPack::select32(blocks, (uint64_t)index, bits, mask);
    }
    assert(format == SegFieldInfo::ORD_PREDICTED);
    uint64_t block = (uint64_t)index / BLOCK_SIZE;
    uint32_t rankInBlock = (uint32_t)((uint64_t)index % BLOCK_SIZE);
    PredictedBlockInfo info = blockInfo(block);
    uint32_t residual = LinearPack::select32(blocks + info.payloadOffset,
                                             rankInBlock, info.bits,
                                             LinearPack::mask32(info.bits));
    int64_t value = OrdColumnFormat::predict(info, rankInBlock) + residual;
    assert(value >= 0 && value <= INT32_MAX);
    return (uint32_t)value;
  }

public:
  OrdColReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo)
      : docs(postingsReader, fieldInfo) {
    nvalues = fieldInfo.numValues;
    maxdoc = postingsReader.maxDoc();
    format = fieldInfo.ordFormat;
    indexing = fieldInfo.ordIndexing;
    bits = (uint8_t)fieldInfo.ordBits;
    mask = LinearPack::mask32(bits);
    multi = fieldInfo.monoLoc.offset() > 0;
    assert(format == SegFieldInfo::ORD_DIRECT || format == SegFieldInfo::ORD_PREDICTED);
    assert(indexing == SegFieldInfo::ORD_DOCID || indexing == SegFieldInfo::ORD_RANK);
    assert(!multi || indexing == SegFieldInfo::ORD_RANK);
    columnIS = postingsReader.getInputStreamSeek(fieldInfo.columnLoc);
    blocks = columnIS.ptr();
    if (format == SegFieldInfo::ORD_PREDICTED) {
      blockMeta = blocks + fieldInfo.columnMetaOff;
    }
    if (multi) {
      endValueRanks.emplace(postingsReader, fieldInfo.monoLoc,
                            fieldInfo.monoMetaOff, fieldInfo.docsWithField);
    }
  }

  // Raw direct view used by low-level codecs and format tests.
  OrdColReader(const char* encoded, int32_t count, uint8_t bits)
      : docs(count), blocks(encoded), nvalues(count), maxdoc(count),
        format(SegFieldInfo::ORD_DIRECT), indexing(SegFieldInfo::ORD_DOCID),
        bits(bits), mask(LinearPack::mask32(bits)) {}

  int64_t numValues() const noexcept {
    return nvalues;
  }

  int64_t encodedValues() const noexcept {
    return indexing == SegFieldInfo::ORD_DOCID ? maxdoc : nvalues;
  }

  bool multiValued() const noexcept {
    return multi;
  }

  bool docIdIndexed() const noexcept {
    return indexing == SegFieldInfo::ORD_DOCID;
  }

  const DocsReader& docsReader() const noexcept {
    return docs;
  }

  std::pair<int64_t, int64_t> getStartEndValueRank(int32_t docRank) const {
    assert(endValueRanks);
    return endValueRanks->valuesAt(docRank);
  }

  int32_t ordAt(int32_t doc) const {
    assert(doc >= 0 && doc < maxdoc);
    assert(!multi);
    uint32_t ord;
    if (indexing == SegFieldInfo::ORD_DOCID) {
      ord = valueAt(doc);
    } else if (!docs.hasBitset()) {
      ord = valueAt(doc);
    } else {
      screaming::BitSet::Iterator iter(docs.bitset());
      if (iter.advance(doc) != doc) return 0;
      ord = valueAt(iter.rank());
    }
    assert(ord <= INT32_MAX);
    return (int32_t)ord;
  }

  int32_t firstOrdAt(int32_t doc) const {
    if (!multi) return ordAt(doc);
    assert(doc >= 0 && doc < maxdoc);
    assert(indexing == SegFieldInfo::ORD_RANK);

    int32_t docRank;
    if (!docs.hasBitset()) {
      docRank = doc;
    } else {
      screaming::BitSet::Iterator iter(docs.bitset());
      if (iter.advance(doc) != doc) return 0;
      docRank = iter.rank();
    }
    auto [start, end] = getStartEndValueRank(docRank);
    assert(start < end);
    uint32_t ord = valueAt(start);
    assert(ord <= INT32_MAX);
    return (int32_t)ord;
  }

  class PointOrds {
    const OrdColReader& col;

  public:
    explicit PointOrds(const OrdColReader& col) : col(col) {}

    int32_t valueAt(int64_t index) const {
      uint32_t ord = col.valueAt(index);
      assert(ord <= INT32_MAX);
      return (int32_t)ord;
    }
  };

  class BulkOrds {
    const OrdColReader& col;
    int64_t decodedStart = -1;
    int64_t decodedEnd = -1;
    uint32_t decoded[BULK_SIZE];

    void decode(int64_t index) {
      decodedStart = (index / BULK_SIZE) * BULK_SIZE;
      decodedEnd = std::min(decodedStart + BULK_SIZE, col.encodedValues());
      uint32_t count = (uint32_t)(decodedEnd - decodedStart);
      if (col.format == SegFieldInfo::ORD_DIRECT) {
        LinearPack::unpack128(col.blocks, (uint64_t)decodedStart, count,
                              col.bits, col.mask, decoded);
        return;
      }
      uint64_t block = (uint64_t)decodedStart / BLOCK_SIZE;
      uint32_t rankInBlock = (uint32_t)((uint64_t)decodedStart % BLOCK_SIZE);
      PredictedBlockInfo info = col.blockInfo(block);
      LinearPack::unpack128(col.blocks + info.payloadOffset, rankInBlock, count,
                            info.bits, LinearPack::mask32(info.bits), decoded);
      for (uint32_t i = 0; i < count; i++) {
        int64_t value = OrdColumnFormat::predict(info, rankInBlock + i) + decoded[i];
        assert(value >= 0 && value <= INT32_MAX);
        decoded[i] = (uint32_t)value;
      }
    }

  public:
    explicit BulkOrds(const OrdColReader& col) : col(col) {}

    int32_t valueAt(int64_t index) {
      assert(index >= 0 && index < col.encodedValues());
      if (index < decodedStart || index >= decodedEnd) decode(index);
      uint32_t ord = decoded[index - decodedStart];
      assert(ord <= INT32_MAX);
      return (int32_t)ord;
    }
  };

  class Iterator {
    const OrdColReader& col;
    screaming::BitSet::Iterator docsIter;
    BulkOrds values_;
    int32_t docRank = -1;
    int32_t doc = -1;
    int32_t maxRank = 0;
    bool dense = false;

  public:
    explicit Iterator(const OrdColReader& col)
        : col(col), docsIter(col.docs.bitset()), values_(col) {
      maxRank = col.docs.numDocs();
      dense = !col.docs.hasBitset();
    }

    int32_t docId() const noexcept { return doc; }
    int32_t rank() const noexcept { return docRank; }
    BulkOrds& values() noexcept { return values_; }

    int32_t value() {
      assert(!col.multi);
      int64_t index = col.indexing == SegFieldInfo::ORD_DOCID ? doc : docRank;
      return values_.valueAt(index);
    }

    int32_t advance(int32_t target) {
      if (dense) {
        if (target >= maxRank) {
          doc = ENDDOC;
        } else {
          doc = docRank = target;
        }
      } else {
        doc = docsIter.advance(target);
        docRank = doc == ENDDOC ? maxRank : docsIter.rank();
      }
      return doc;
    }

    int32_t next() {
      if (docRank + 1 >= maxRank) {
        doc = ENDDOC;
        return doc;
      }
      docRank++;
      doc = dense ? doc + 1 : docsIter.next();
      return doc;
    }
  };

  static void getValues(MemPool& pool, PostingsReader& postingsReader,
                        SegFieldInfo& fieldInfo, std::ranges::input_range auto&& sortedIds,
                        auto&& callback) {
    unused(pool);
    OrdColReader reader(postingsReader, fieldInfo);
    Iterator iter(reader);
    int32_t found = -1;
    size_t inputIndex = 0;
    for (int32_t doc : sortedIds) {
      if (found < doc) found = iter.advance(doc);
      if (found == doc) {
        if (!reader.multiValued()) {
          callback(inputIndex, doc, iter.value(), 0, 1);
        } else {
          auto [start, end] = reader.getStartEndValueRank(iter.rank());
          for (int64_t i = 0; i < end - start; i++) {
            callback(inputIndex, doc, iter.values().valueAt(start + i), i, end - start);
          }
        }
      } else if (found == ENDDOC) {
        break;
      }
      inputIndex++;
    }
  }

  static void getSingleValues(MemPool& pool, PostingsReader& postingsReader,
                              SegFieldInfo& fieldInfo,
                              std::ranges::input_range auto&& sortedIds,
                              auto&& callback) {
    unused(pool);
    OrdColReader reader(postingsReader, fieldInfo);
    assert(!reader.multiValued());
    size_t inputIndex = 0;
    for (int32_t doc : sortedIds) {
      int32_t ord = reader.ordAt(doc);
      if (ord != 0) callback(inputIndex, doc, ord);
      inputIndex++;
    }
  }

  static void getSingleValues(MemPool& pool, PostingsReader& postingsReader,
                              std::string_view field,
                              std::ranges::input_range auto&& sortedIds,
                              auto&& callback) {
    FieldReader fieldReader(pool, postingsReader);
    if (!fieldReader.seek(field)) return;
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    getSingleValues(pool, postingsReader, fieldInfo,
                    std::forward<decltype(sortedIds)>(sortedIds),
                    std::forward<decltype(callback)>(callback));
  }
};

} // namespace solux
