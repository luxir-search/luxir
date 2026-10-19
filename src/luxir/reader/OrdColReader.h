// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <ranges>
#include <string_view>
#include <utility>

#include "DocsReader.h"
#include "IntColReader.h"
#include "PostingsReader.h"
#include "luxir/codec/LinearPack.h"
#include "luxir/codec/OrdColumnFormat.h"
#include "luxir/util/log.h"

namespace luxir {

class OrdColReader {
public:
  static constexpr uint32_t BLOCK_SIZE = OrdColumnFormat::BLOCK_SIZE;
  static constexpr uint32_t BULK_SIZE = OrdColumnFormat::BULK_SIZE;
  // The measured point-to-bulk cost ratio spans 5.5x to 10x, putting the raw
  // 128-value crossover at 13-24 hits. Use the conservative end by default.
  static constexpr int32_t DEFAULT_BULK_MIN_HITS = 24;
  using PredictedBlockInfo = OrdColumnFormat::PredictedBlockInfo;
  static constexpr int32_t ENDDOC = std::numeric_limits<int32_t>::max();
  static constexpr int64_t ENDINDEX = std::numeric_limits<int64_t>::max();

  struct ForEachOrdStats {
    int64_t pointLoads = 0;
    int64_t bulkLoads = 0;
    int32_t pointBlocks = 0;
    int32_t bulkBlocks = 0;
  };

private:
  inline static std::atomic<int32_t> bulkMinHitsOverride = -1;
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

  uint32_t directValueAt(int64_t index) const {
    assert(index >= 0 && index < encodedValues());
    assert(format == SegFieldInfo::ORD_DIRECT);
    return LinearPack::select32(blocks, (uint64_t)index, bits, mask);
  }

  uint32_t predictedValueAt(int64_t index) const {
    assert(index >= 0 && index < encodedValues());
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

  uint32_t valueAt(int64_t index) const {
    return format == SegFieldInfo::ORD_DIRECT ? directValueAt(index)
                                              : predictedValueAt(index);
  }

  // Point-decodes each doc `visitDocs` hands its visitor (ascending, in a
  // docid-indexed column) and keeps the docs with a value: docsOut and
  // ordsOut get the present pairs in order, and their count is returned. The
  // format dispatch sits outside the visit, and the loads do not depend on
  // each other, so a sparse domain's column misses overlap.
  template <class VisitDocs>
  int32_t presentOrds(VisitDocs&& visitDocs, int32_t* docsOut,
                      int32_t* ordsOut) const {
    assert(indexing == SegFieldInfo::ORD_DOCID);
    int32_t present = 0;
    auto keep = [&](int32_t doc, uint32_t ord) LUXIR_INLINE {
      assert(ord <= INT32_MAX);
      docsOut[present] = doc;
      ordsOut[present] = (int32_t)ord;
      present += ord != 0;
    };
    if (format == SegFieldInfo::ORD_DIRECT) {
      // Locals, not members: the output stores could alias the reader, which
      // would reload the mask on every doc.
      const char* base = blocks;
      uint8_t width = bits;
      uint32_t valueMask = mask;
      visitDocs([&](int32_t doc) LUXIR_INLINE {
        assert(doc >= 0 && doc < encodedValues());
        keep(doc, LinearPack::select32(base, (uint64_t)doc, width, valueMask));
      });
    } else {
      visitDocs([&](int32_t doc) LUXIR_INLINE {
        keep(doc, predictedValueAt(doc));
      });
    }
    return present;
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

  // The `count` values from index `start`, a multiple of BULK_SIZE, in one
  // bulk unpack (count <= BULK_SIZE).
  void unpack(int64_t start, uint32_t count, uint32_t* out) const {
    assert(start >= 0 && start % BULK_SIZE == 0 && count <= BULK_SIZE);
    assert(start + count <= encodedValues());
    if (format == SegFieldInfo::ORD_DIRECT) {
      LinearPack::unpack128(blocks, (uint64_t)start, count, bits, mask, out);
      return;
    }
    uint64_t block = (uint64_t)start / BLOCK_SIZE;
    uint32_t rankInBlock = (uint32_t)((uint64_t)start % BLOCK_SIZE);
    PredictedBlockInfo info = blockInfo(block);
    LinearPack::unpack128(blocks + info.payloadOffset, rankInBlock, count,
                          info.bits, LinearPack::mask32(info.bits), out);
    for (uint32_t i = 0; i < count; i++) {
      int64_t value = OrdColumnFormat::predict(info, rankInBlock + i) + out[i];
      assert(value >= 0 && value <= INT32_MAX);
      out[i] = (uint32_t)value;
    }
  }

  // presentOrds over ascending `docs`.
  int32_t presentOrdsAt(std::span<const int32_t> docs, int32_t* docsOut,
                        int32_t* ordsOut) const {
    return presentOrds([&](auto&& visit) LUXIR_INLINE {
      for (int32_t doc : docs) visit(doc);
    }, docsOut, ordsOut);
  }

  // presentOrds over the docs set in `words`, where bit i of words[w] is doc
  // docBase + 64 * w + i: the bit scan feeds the decode directly, with no
  // list of docs in between.
  int32_t presentOrdsAtBits(std::span<const uint64_t> words, int32_t docBase,
                            int32_t* docsOut, int32_t* ordsOut) const {
    return presentOrds([&](auto&& visit) LUXIR_INLINE {
      for (size_t w = 0; w < words.size(); w++) {
        int32_t wordBase = docBase + (int32_t)(w * 64);
        for (uint64_t bitsLeft = words[w]; bitsLeft != 0;
             bitsLeft &= bitsLeft - 1) {
          visit(wordBase + std::countr_zero(bitsLeft));
        }
      }
    }, docsOut, ordsOut);
  }

  class BulkOrds {
    const OrdColReader& col;
    int64_t decodedStart = -1;
    int64_t decodedEnd = -1;
    uint32_t decoded[BULK_SIZE];

    void decode(int64_t index) {
      decodedStart = (index / BULK_SIZE) * BULK_SIZE;
      decodedEnd = std::min(decodedStart + BULK_SIZE, col.encodedValues());
      col.unpack(decodedStart, (uint32_t)(decodedEnd - decodedStart), decoded);
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

public:
  static int32_t configuredBulkMinHits() {
    int32_t override = bulkMinHitsOverride.load();
    if (override >= 0) return override;

    const char* configured = std::getenv("LUXIR_ORD_BULK_MIN_HITS");
    if (configured == nullptr || *configured == '\0') {
      return DEFAULT_BULK_MIN_HITS;
    }
    std::string_view text(configured);
    int32_t value = -1;
    auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error == std::errc() && end == text.data() + text.size()
        && value >= 0 && value <= (int32_t)BULK_SIZE) {
      return value;
    }
    LOG_WARN("Ignoring invalid LUXIR_ORD_BULK_MIN_HITS='{}'; expected 0..{}",
             configured, BULK_SIZE);
    return DEFAULT_BULK_MIN_HITS;
  }

  static int32_t setBulkMinHitsForTests(int32_t value) {
    assert(value >= -1 && value <= (int32_t)BULK_SIZE);
    return bulkMinHitsOverride.exchange(value);
  }

  // Whether `hits` docs of a `span`-doc block (span <= BULK_SIZE; a tail
  // block is shorter) decode cheaper with one bulk unpack than by point.
  static bool bulkBlock(int32_t hits, int32_t span, int32_t bulkMinHits) {
    return (int64_t)hits * BULK_SIZE >= (int64_t)bulkMinHits * span;
  }

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
                        const SegFieldInfo& fieldInfo, std::ranges::input_range auto&& sortedIds,
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
                              const SegFieldInfo& fieldInfo,
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


};

} // namespace luxir
