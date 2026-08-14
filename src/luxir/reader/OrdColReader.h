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
  // Four decode blocks are examined together to amortize loop overhead, but
  // each 128-value block makes its own bulk-versus-point decision.
  static constexpr int32_t ORD_BATCH_SIZE = (int32_t)BULK_SIZE * 4;
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

private:
  static uint64_t lowBitsMask(int32_t bits) {
    assert(bits >= 0 && bits <= 64);
    if (bits == 0) return 0;
    if (bits == 64) return ~0ULL;
    return (1ULL << bits) - 1;
  }

  // invertMask (0 or ~0ULL) XORs each domain word before use, so the same walk
  // serves a domain and its complement. The window mask is applied after the
  // inversion, so inverted garbage past maxDoc is never visited.
  static uint64_t windowWord(const screaming::FixedBitSet& domainBits,
                             int32_t word,
                             int32_t windowEnd, uint64_t invertMask) {
    uint64_t bits = domainBits.words[word] ^ invertMask;
    int32_t validBits = windowEnd - (word << 6);
    if (validBits < 64) bits &= lowBitsMask(validBits);
    return bits;
  }

  static void visitSetBits(const screaming::FixedBitSet& domainBits,
                           int32_t windowStart, int32_t windowEnd,
                           uint64_t invertMask, auto&& visitor) {
    int32_t wordStart = windowStart >> 6;
    int32_t wordEnd = (windowEnd + 63) >> 6;
    for (int32_t word = wordStart; word < wordEnd; word++) {
      uint64_t bits = windowWord(domainBits, word, windowEnd, invertMask);
      int32_t docBase = word << 6;
      while (bits != 0) {
        int32_t bit = (int32_t)std::countr_zero(bits);
        visitor(docBase + bit);
        bits &= bits - 1;
      }
    }
  }

  static bool bulkBlock(int32_t cardinality, int32_t blockBits,
                        int32_t bulkMinHits) {
    return (int64_t)cardinality * BULK_SIZE
        >= (int64_t)bulkMinHits * blockBits;
  }

  void forEachPointOrd(auto&& visitDocs, int64_t& missing_num,
                       auto&& callback, ForEachOrdStats* stats) const {
    PointOrds values(*this);
    if (indexing == SegFieldInfo::ORD_DOCID) {
      assert(!multi);
      visitDocs([&](int32_t docid) {
        int32_t ord = values.valueAt(docid);
        if (stats) stats->pointLoads++;
        if (ord != 0) callback(docid, ord);
        else missing_num++;
      });
      return;
    }

    screaming::BitSet::Iterator docsIter(docs.bitset());
    bool dense = !docs.hasBitset();
    int32_t found = -1;
    visitDocs([&](int32_t docid) {
      int32_t docRank;
      if (dense) {
        docRank = docid;
      } else {
        if (found < docid) found = docsIter.advance(docid);
        if (found != docid) {
          missing_num++;
          return;
        }
        docRank = docsIter.rank();
      }
      if (!multi) {
        callback(docid, values.valueAt(docRank));
        if (stats) stats->pointLoads++;
        return;
      }
      auto [start, end] = getStartEndValueRank(docRank);
      for (int64_t rank = start; rank < end; rank++) {
        callback(docid, values.valueAt(rank));
        if (stats) stats->pointLoads++;
      }
    });
  }

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

  void forEachOrd(std::span<int32_t> domainDocs, int64_t& missing_num,
                  auto&& callback, ForEachOrdStats* stats = nullptr) const {
    forEachPointOrd(
        [&](auto&& visitor) {
          for (int32_t docid : domainDocs) visitor(docid);
        },
        missing_num, callback, stats);
  }

  // invertMask ~0ULL walks the domain's complement without materializing a
  // second bitset; the adaptive bulk/point decisions see complement
  // cardinalities through the same inverted word reads.
  void LUXIR_NOINLINE forEachDocIdBitOrd(
      const screaming::FixedBitSet& domainBits, int32_t maxDoc,
      int32_t bulkMinHits, int64_t& missing_num,
      auto&& callback, ForEachOrdStats* stats = nullptr,
      uint64_t invertMask = 0) const {
    assert(maxDoc == maxdoc);
    assert(bulkMinHits >= 0 && bulkMinHits <= (int32_t)BULK_SIZE);
    assert(domainBits.size() >= maxDoc);
    assert(indexing == SegFieldInfo::ORD_DOCID);
    assert(!multi);
    PointOrds pointValues(*this);
    BulkOrds bulkValues(*this);
    auto visitRange = [&](int32_t start, int32_t end, bool useBulk) {
      visitSetBits(domainBits, start, end, invertMask, [&](int32_t docid) {
        int32_t ord = useBulk
            ? bulkValues.valueAt(docid) : pointValues.valueAt(docid);
        if (stats) {
          if (useBulk) stats->bulkLoads++;
          else stats->pointLoads++;
        }
        if (ord != 0) callback(docid, ord);
        else missing_num++;
      });
    };

    for (int32_t batchStart = 0; batchStart < maxDoc;
         batchStart += ORD_BATCH_SIZE) {
      int32_t batchEnd = std::min(batchStart + ORD_BATCH_SIZE, maxDoc);
      std::array<int32_t, 4> cardinalities{};
      std::array<int32_t, 4> blockEnds;
      std::array<bool, 4> useBulk;
      int32_t wordStart = batchStart >> 6;
      int32_t wordEnd = (batchEnd + 63) >> 6;
      for (int32_t word = wordStart; word < wordEnd; word++) {
        cardinalities[(word - wordStart) >> 1] += (int32_t)std::popcount(
            windowWord(domainBits, word, batchEnd, invertMask));
      }
      int32_t nonemptyBlocks = 0;
      int32_t bulkBlocks = 0;
      for (int32_t i = 0; i < 4; i++) {
        int32_t blockStart = batchStart + i * (int32_t)BULK_SIZE;
        int32_t blockEnd = std::min(
            blockStart + (int32_t)BULK_SIZE, batchEnd);
        blockEnds[i] = blockEnd;
        if (blockStart >= batchEnd) {
          useBulk[i] = false;
          continue;
        }
        int32_t cardinality = cardinalities[i];
        useBulk[i] = cardinality != 0 && bulkBlock(
            cardinality, blockEnd - blockStart, bulkMinHits);
        if (cardinality != 0) {
          nonemptyBlocks++;
          if (useBulk[i]) bulkBlocks++;
        }
      }

      if (stats) {
        stats->bulkBlocks += bulkBlocks;
        stats->pointBlocks += nonemptyBlocks - bulkBlocks;
      }
      if (bulkBlocks == 0) {
        visitRange(batchStart, batchEnd, false);
        continue;
      }
      if (bulkBlocks == nonemptyBlocks) {
        visitRange(batchStart, batchEnd, true);
        continue;
      }

      for (int32_t i = 0; i < 4; i++) {
        int32_t blockStart = batchStart + i * (int32_t)BULK_SIZE;
        int32_t blockEnd = blockEnds[i];
        if (blockStart >= blockEnd) break;
        visitRange(blockStart, blockEnd, useBulk[i]);
      }
    }
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
    FieldReader fieldReader(postingsReader);
    if (!fieldReader.seek(field)) return;
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    getSingleValues(pool, postingsReader, fieldInfo,
                    std::forward<decltype(sortedIds)>(sortedIds),
                    std::forward<decltype(callback)>(callback));
  }
};

} // namespace luxir
