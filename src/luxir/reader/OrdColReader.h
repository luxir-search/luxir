// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
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
#include "ReaderErrors.h"
#include "IntColReader.h"
#include "PostingsReader.h"
#include "luxir/codec/LinearPack.h"
#include "luxir/codec/OrdColumnFormat.h"
#include "luxir/util/log.h"

namespace luxir {

// Reads a string or ID column as ordinals into the field's term dictionary.
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
  DocsReader docs;  // Maps sparse docs to ranks.
  const char* blocks = nullptr;  // Packed ordinal values.
  const char* blockMeta = nullptr;  // The prediction metadata, if present.
  MonoReader endValueRanks;  // Maps each doc rank to its end value rank.
  int64_t values = 0;  // Number of values in the column.
  int32_t maxDoc = 0;
  field_parts::OrdMode mode;
  field_parts::OrdIndexing indexing;
  uint8_t width = 0;  // Bits per ordinal in the direct layout.
  uint32_t mask = 0;  // Extracts one ordinal from packed bits.
  bool array = false;  // Values retain their input order and repeats.

  PredictedBlockInfo blockInfo(uint64_t block) const {
    PredictedBlockInfo info;
    memcpy(&info, blockMeta + block * sizeof(PredictedBlockInfo), sizeof(info));
    return info;
  }

  uint32_t directValueAt(int64_t index) const {
    assert(index >= 0 && index < encodedValues());
    assert(mode == field_parts::OrdMode::DIRECT);
    return LinearPack::select32(blocks, (uint64_t)index, width, mask);
  }

  uint32_t predictedValueAt(int64_t index) const {
    assert(index >= 0 && index < encodedValues());
    assert(mode == field_parts::OrdMode::PREDICTED);
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
    return mode == field_parts::OrdMode::DIRECT ? directValueAt(index)
                                              : predictedValueAt(index);
  }

  // Point-decodes each doc `visitDocs` hands its visitor (ascending, in a
  // docid-indexed column) and keeps the docs with a value: docsOut and
  // ordsOut get the present pairs in order, and their count is returned. The
  // mode dispatch sits outside the visit, and the loads do not depend on
  // each other, so a sparse domain's column misses overlap.
  template <class VisitDocs>
  int32_t presentOrds(VisitDocs&& visitDocs, int32_t* docsOut,
                      int32_t* ordsOut) const {
    assert(indexing == field_parts::OrdIndexing::DOC_ID);
    int32_t present = 0;
    auto keep = [&](int32_t doc, uint32_t ord) LUXIR_INLINE {
      assert(ord <= INT32_MAX);
      docsOut[present] = doc;
      ordsOut[present] = (int32_t)ord;
      present += ord != 0;
    };
    if (mode == field_parts::OrdMode::DIRECT) {
      // Locals, not members: the output stores could alias the reader, which
      // would reload the mask on every doc.
      const char* base = blocks;
      uint8_t packedWidth = width;
      uint32_t valueMask = mask;
      visitDocs([&](int32_t doc) LUXIR_INLINE {
        assert(doc >= 0 && doc < encodedValues());
        keep(doc, LinearPack::select32(base, (uint64_t)doc, packedWidth, valueMask));
      });
    } else {
      visitDocs([&](int32_t doc) LUXIR_INLINE {
        keep(doc, predictedValueAt(doc));
      });
    }
    return present;
  }

public:
  OrdColReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : docs(reader, fieldInfo), values(fieldInfo.numValues), maxDoc(reader.maxDoc()),
        array((fieldInfo.flags & FieldType::MULTI_ARRAY) != 0) {
    const auto* ord = fieldInfo.ordColumn();
    if (!ord) missingColumn(fieldInfo, "ordinal");
    mode = ord->mode;
    indexing = ord->indexing;
    width = OrdColumnFormat::width(fieldInfo.indexedTotals().numTerms);
    mask = LinearPack::mask32(width);
    blocks = reader.core().slice(ord->values()).data();
    if (auto metadata = fieldInfo.ordBlockMetadata(maxDoc)) {
      blockMeta = reader.core().slice(metadata).data();
    }
    endValueRanks = MonoReader::openEndValueRanks(reader, fieldInfo);
  }

  // Raw direct view used by low-level codecs and format tests.
  OrdColReader(const char* encoded, int32_t numDocs, uint8_t width)
      : docs(numDocs), blocks(encoded), values(numDocs), maxDoc(numDocs),
        mode(field_parts::OrdMode::DIRECT), indexing(field_parts::OrdIndexing::DOC_ID),
        width(width), mask(LinearPack::mask32(width)) {}

  int64_t numValues() const noexcept {
    return values;
  }

  int64_t encodedValues() const noexcept {
    return field_parts::OrdColumn::encodedCount(indexing, maxDoc, values);
  }

  bool multiArray() const noexcept { return array; }

  bool multiValued() const noexcept {
    return endValueRanks.has_value();
  }

  bool docIdIndexed() const noexcept {
    return indexing == field_parts::OrdIndexing::DOC_ID;
  }

  const DocsReader& docsReader() const noexcept {
    return docs;
  }

  std::pair<int64_t, int64_t> getStartEndValueRank(int32_t docRank) const {
    assert(endValueRanks);
    return endValueRanks.valuesAt(docRank);
  }

  int32_t ordAt(int32_t doc) const {
    assert(doc >= 0 && doc < maxDoc);
    assert(!multiValued());
    uint32_t ord;
    if (indexing == field_parts::OrdIndexing::DOC_ID) {
      ord = valueAt(doc);
    } else if (docs.allRows()) {
      ord = valueAt(doc);
    } else {
      int32_t rank = docs.rankOf(doc);
      if (rank < 0) return 0;
      ord = valueAt(rank);
    }
    assert(ord <= INT32_MAX);
    return (int32_t)ord;
  }

  // Minimum term ordinal, independent of the stored sequence order.
  int32_t minOrdAt(int32_t doc) const {
    if (multiValued() && array) return selectedArrayOrdAt(doc, false);
    return firstOrdAt(doc);
  }

  // First stored ordinal: the minimum for single-valued and set columns.
  int32_t firstOrdAt(int32_t doc) const {
    if (!multiValued()) return ordAt(doc);
    assert(doc >= 0 && doc < maxDoc);
    assert(indexing == field_parts::OrdIndexing::RANK);

    int32_t docRank = doc;
    if (!docs.allRows()) {
      docRank = docs.rankOf(doc);
      if (docRank < 0) return 0;
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
    if (mode == field_parts::OrdMode::DIRECT) {
      LinearPack::unpack128(blocks, (uint64_t)start, count, width, mask, out);
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

  int32_t LUXIR_NOINLINE selectedArrayOrdAt(int32_t doc, bool descending) const {
    assert(multiValued() && array);
    int32_t rank = doc;
    if (!docs.allRows()) {
      rank = docs.rankOf(doc);
      if (rank < 0) return 0;
    }
    auto [start, end] = getStartEndValueRank(rank);
    BulkOrds ords(*this);
    int32_t selected = ords.valueAt(start);
    for (int64_t i = start + 1; i < end; i++) {
      int32_t ord = ords.valueAt(i);
      selected = descending ? std::max(selected, ord) : std::min(selected, ord);
    }
    return selected;
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
        : col(col), docsIter(col.docs.bitmapIterator()), values_(col) {
      maxRank = col.docs.docsWithField();
      dense = col.docs.allRows();
    }

    int32_t docId() const noexcept { return doc; }
    int32_t rank() const noexcept { return docRank; }
    BulkOrds& values() noexcept { return values_; }

    int32_t value() {
      assert(!col.multiValued());
      int64_t index = col.indexing == field_parts::OrdIndexing::DOC_ID ? doc : docRank;
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

  static void getValues(PostingsReader& postingsReader,
                        const DecodedFieldRecord& fieldInfo, std::ranges::input_range auto&& sortedIds,
                        auto&& callback) {
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

  static void getSingleValues(PostingsReader& postingsReader,
                              const DecodedFieldRecord& fieldInfo,
                              std::ranges::input_range auto&& sortedIds,
                              auto&& callback) {
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
