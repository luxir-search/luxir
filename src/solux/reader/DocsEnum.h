#pragma once

#include "TermsEnum.h"
#include "SkipStats.h"
#include "solux/codec/Codec.h"
#include "solux/codec/StreamVByte.h"
#include "solux/util/DecodedSuccessor.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace solux {
enum class DocsEnumTier : uint8_t { DOCS, FREQS, POSITIONS };

template<DocsEnumTier Tier>
class BasicDocsEnum;

using DocsOnlyEnum = BasicDocsEnum<DocsEnumTier::DOCS>;
using DocsFreqEnum = BasicDocsEnum<DocsEnumTier::FREQS>;
using DocsPosEnum = BasicDocsEnum<DocsEnumTier::POSITIONS>;

class PosEnum;

// Tier-erased metadata shared by every postings cursor. This class contains no
// decoded-doc, term-frequency, or position cursor state; it only exposes the
// immutable postings layout needed to build impact indexes.
class DocsEnumMeta {
  friend class PosEnum;

protected:
  InputStream docIS;
  bool hasFreqs;
  bool hasPositions;
  bool hasNorms;
  // uint8_t currentL1PackedBlocks = 0;
  // The per-group packed-block count is one byte in every L1 group header,
  // right after the vint15 lastDoc delta and vlong15 group byte length.
  // The sampled L0 directory follows it.
  int32_t docfreq;
  int64_t ttf;
  int64_t docsSize;
  int64_t startOfDocs;
  int64_t endOfDocs;
  const char* termImpactFrontierPtr = nullptr;
  uint32_t termImpactFrontierLen = 0;
  int64_t termOrdinal = -1;
  int32_t numDocBlocks = 0;
  int32_t termPackedBlocks = 0;

public:
  /// sentinel value used for docs
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  // The skip structure has L0 per-block headers and L1 group headers every
  // L1_PERIOD blocks. Each L1 header also samples L0 resume points within its
  // group; there is deliberately no coarser level above L1.
  static constexpr int32_t L1_PERIOD = 32;
  static constexpr int32_t L1_DOCS = L1_PERIOD * Postings::DOCS_BLOCK_SIZE;
  static inline bool disableL0CheckpointsForTests = false;

  struct ImpactFrontiers {
    std::vector<int32_t> offsets;
    std::vector<int32_t> tfs;
    std::vector<int32_t> norms;

    void clear() {
      offsets.resize(0);
      tfs.resize(0);
      norms.resize(0);
    }

    int32_t count(int32_t block) const {
      assert(block >= 0);
      assert(block + 1 < (int32_t) offsets.size());
      return offsets[(size_t) block + 1] - offsets[(size_t) block];
    }
  };

  struct GroupBlockImpactScratch {
    static constexpr int32_t FRONTIER_CAP = 256;
    int32_t frontierTfs[FRONTIER_CAP];
    int32_t frontierNorms[FRONTIER_CAP];
  };

  struct GroupImpactCursor {
    const char* next = nullptr;
    uint32_t prevGroupLastDoc = 0;
    int32_t nextGroup = 0;
    bool initialized = false;
  };

  struct GroupImpactHeader {
    int32_t group = 0;
    int32_t lastDoc = 0;
    int32_t packedBlockCount = 0;
    int32_t spanMaxTf = 1;
    int32_t spanMinNorm = 0;
    int64_t bodyOffset = 0;
    int32_t baseLastDoc = 0;
    bool frontierSpilled = false;
    std::span<const uint8_t> frontierNorms;
    std::span<const char> frontierTfBytes;
    uint32_t frontierTfWidth = 0;
  };

  struct GroupImpacts {
    std::vector<int32_t> lastDocs;
    std::vector<int32_t> packedBlockCounts;
    std::vector<int32_t> spanMaxTfs;
    std::vector<int32_t> spanMinNorms;
    std::vector<int64_t> bodyOffsets;
    std::vector<int32_t> baseLastDocs;
    ImpactFrontiers frontiers;
  };

protected:
  struct L0Checkpoint {
    uint32_t key = 0;
    uint32_t bodyOffset = 0;
    int32_t blockIdxInGroup = 0;
  };

  static inline bool disableL0Checkpoints =
      std::getenv("SOLUX_DISABLE_L0_CHECKPOINTS") != nullptr;

  // Consult pre-filter: a block holds exactly DOCS_BLOCK_SIZE docs, so the
  // doc-id distance to the target bounds the block distance. Below one
  // default writer stride (8 blocks) no jump is possible and the consult is
  // skipped without touching the directory. Conservative only: a smaller
  // written stride just forgoes marginal jumps, never selects a wrong one.
  static constexpr int32_t L0_CHECKPOINT_MIN_SKIP_DOCS =
      8 * Postings::DOCS_BLOCK_SIZE;

  // A jump must skip at least this many headers to beat its own cost: a
  // header parse is a couple of cheap vint reads, so short hops lose to the
  // walk (measured on dense word-probed conjunctions).
  static constexpr int32_t L0_CHECKPOINT_MIN_JUMP_BLOCKS = 4;

  // Branchless over the (at most a few) entries: data-dependent early exits
  // mispredict on probe workloads and cost more than the loop they save.
  static bool consultL0Checkpoints(const char* entries, uint8_t entryCount,
                                   uint32_t target, int32_t blockInGroup,
                                   L0Checkpoint& selected) {
    if (disableL0Checkpoints || disableL0CheckpointsForTests) {
      return false;
    }
    int32_t minBlockIdx = blockInGroup + L0_CHECKPOINT_MIN_JUMP_BLOCKS;
    uint64_t best = 0;
    for (uint8_t i = 0; i < entryCount; i++) {
      uint64_t entry;
      static_assert(std::endian::native == std::endian::little);
      memcpy(&entry, entries + (size_t) i * sizeof(entry), sizeof(entry));
      int32_t blockIdx = (int32_t) ((entry >> 24) & 0xffu);
      bool usable = (uint32_t) (entry >> 32) < target && blockIdx >= minBlockIdx;
      best = usable ? entry : best;
    }
    if (best == 0) {
      return false;
    }
    selected = L0Checkpoint{
        .key = (uint32_t) (best >> 32),
        .bodyOffset = (uint32_t) (best & 0xffffffu),
        .blockIdxInGroup = (int32_t) ((best >> 24) & 0xffu)};
    assert(selected.blockIdxInGroup > 0
           && selected.blockIdxInGroup < L1_PERIOD);
    return true;
  }

  explicit DocsEnumMeta(const TermsEnum::PostingsState& state)
      : docIS(state.docIS), hasFreqs(state.hasFreqs),
        hasPositions(state.hasPositions), hasNorms(state.hasPositions),
        docfreq(state.docFreq), ttf(state.totalTermFreq),
        docsSize(state.docsEnd - state.docsStart), startOfDocs(state.docsStart),
        endOfDocs(state.docsEnd),
        termImpactFrontierPtr(state.termImpactFrontier.ptr),
        termImpactFrontierLen(state.termImpactFrontier.len),
        termOrdinal(state.termOrdinal),
        termPackedBlocks(state.packedBlockCount) {
    assert(endOfDocs >= startOfDocs);
    if (docsSize != 0) {
      numDocBlocks =
          (docfreq + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
    }
  }

  DocsEnumMeta(const DocsEnumMeta&) = delete;

  int32_t docGroupCount() const {
    return (numDocBlocks + L1_PERIOD - 1) / L1_PERIOD;
  }

  static uint32_t readVint15(const char*& pos, const char* end) {
    assert(pos + 2 <= end);
    uint16_t s = (uint16_t) (uint8_t) pos[0]
                 | (uint16_t) ((uint16_t) (uint8_t) pos[1] << 8);
    pos += 2;
    if ((s & 0x8000u) == 0) {
      return s;
    }
    return (s & 0x7fffu) | (InputStream::readVint(pos, end) << 15);
  }

  static uint64_t readVlong15(const char*& pos, const char* end) {
    assert(pos + 2 <= end);
    uint16_t s = (uint16_t) (uint8_t) pos[0]
                 | (uint16_t) ((uint16_t) (uint8_t) pos[1] << 8);
    pos += 2;
    if ((s & 0x8000u) == 0) {
      return s;
    }
    return (s & 0x7fffull) | (InputStream::readVlong(pos, end) << 15);
  }

  static uint32_t readU16LE(const char* p) {
    uint8_t b[2];
    memcpy(b, p, sizeof(b));
    return (uint32_t) b[0] | ((uint32_t) b[1] << 8);
  }

  static uint32_t readU32LE(const char* p) {
    uint8_t b[4];
    memcpy(b, p, sizeof(b));
    return (uint32_t) b[0] | ((uint32_t) b[1] << 8)
           | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
  }

  static uint32_t readPackedL1Tf(std::span<const char> bytes, uint32_t width,
                                 uint32_t index) {
    assert(width == 2 || width == 4);
    const char* p = bytes.data() + (size_t) index * width;
    assert(p + width <= bytes.data() + bytes.size());
    return width == 2 ? readU16LE(p) : readU32LE(p);
  }

  static void decodePackedL1Frontier(const char*& p, const char* end,
                                     GroupImpactHeader& header) {
    uint32_t code = InputStream::readVint(p, end);
    uint32_t count = code >> 1;
    uint32_t width = (code & 1u) != 0 ? 4u : 2u;
    assert(count > 0 && count <= 256);
    assert(p + count <= end);
    const uint8_t* norms = (const uint8_t*) p;
    p += count;
    size_t tfBytes = (size_t) count * width;
    assert(p + tfBytes <= end);
    const char* tfs = p;
    p += tfBytes;
    header.frontierNorms = {norms, (size_t) count};
    header.frontierTfBytes = {tfs, tfBytes};
    header.frontierTfWidth = width;
    header.spanMinNorm = (int32_t) norms[0];
    header.spanMaxTf =
        (int32_t) readPackedL1Tf(header.frontierTfBytes, width, count - 1);
    header.frontierSpilled = false;
  }

private:
  void initGroupImpactCursor(GroupImpactCursor& cursor) const {
    cursor.next = docsSize == 0 ? nullptr : docIS.ptr(startOfDocs);
    cursor.prevGroupLastDoc = 0;
    cursor.nextGroup = 0;
    cursor.initialized = true;
  }

  GroupImpactHeader parseNextGroupImpactHeader(GroupImpactCursor& cursor) const {
    assert(docsSize != 0);
    if (!cursor.initialized) {
      initGroupImpactCursor(cursor);
    }
    assert(cursor.nextGroup >= 0 && cursor.nextGroup < docGroupCount());
    const char* const streamStart = docIS.ptr(0);
    const char* const end = docIS.ptr(endOfDocs);
    const char* p = cursor.next;
    GroupImpactHeader header;
    header.group = cursor.nextGroup;
    header.baseLastDoc = (int32_t) cursor.prevGroupLastDoc;

    uint32_t groupHeaderLen = InputStream::readVint(p, end);
    const char* groupHeaderEnd = p + groupHeaderLen;
    assert(groupHeaderEnd <= end);
    header.lastDoc =
        (int32_t) (cursor.prevGroupLastDoc + readVint15(p, groupHeaderEnd));
    uint64_t groupByteLen = readVlong15(p, groupHeaderEnd);
    assert(p < groupHeaderEnd);
    header.packedBlockCount = (int32_t) (uint8_t) *p++;
    assert(header.packedBlockCount <= L1_PERIOD);
    assert(p < groupHeaderEnd);
    uint8_t entryCount = (uint8_t) *p++;
    assert(p + (size_t) entryCount * sizeof(uint64_t) <= groupHeaderEnd);
    p += (size_t) entryCount * sizeof(uint64_t);
    if (hasPositions) {
      auto groupCumTfDelta = InputStream::readVint(p, groupHeaderEnd);
      unused(groupCumTfDelta);
    }
    if (hasFreqs && hasNorms) {
      decodePackedL1Frontier(p, groupHeaderEnd, header);
    } else if (hasFreqs) {
      header.spanMaxTf = (int32_t) InputStream::readVint(p, groupHeaderEnd);
    } else if (hasNorms) {
      header.spanMinNorm = (int32_t) InputStream::readVint(p, groupHeaderEnd);
    }
    assert(p == groupHeaderEnd);
    header.bodyOffset = (int64_t) (groupHeaderEnd - streamStart) - startOfDocs;
    cursor.next = groupHeaderEnd + (int64_t) groupByteLen;
    assert(cursor.next <= end);
    cursor.prevGroupLastDoc = (uint32_t) header.lastDoc;
    cursor.nextGroup++;
    return header;
  }

public:
  bool indexHasPositions() const { return hasPositions; }
  int32_t numDocs() { return docfreq; }
  int32_t totalTermFreq() { return ttf; }
  int32_t packedBlockCount() const { return termPackedBlocks; }
  int64_t termOrd() const { return termOrdinal; }
  bool hasTermImpacts() const { return termImpactFrontierPtr != nullptr; }

  std::span<const char> encodedTermImpactFrontier() const {
    return {termImpactFrontierPtr, (size_t) termImpactFrontierLen};
  }

  template <typename Visitor>
  int32_t visitTermImpactFrontier(Visitor&& visitor) const {
    if (!hasTermImpacts()) {
      return 0;
    }
    const char* p = termImpactFrontierPtr;
    const char* end = termImpactFrontierPtr + termImpactFrontierLen;
    uint32_t count = InputStream::readVint(p, end);
    int32_t tf = 0;
    for (uint32_t i = 0; i < count; i++) {
      assert(p < end);
      int32_t norm = (int32_t) (uint8_t) *p++;
      tf += (int32_t) InputStream::readVint(p, end);
      visitor(tf, norm);
    }
    assert(p == end);
    return (int32_t) count;
  }

  int32_t readTermImpactFrontier(std::vector<int32_t>& norms,
                                 std::vector<int32_t>& tfs) const {
    norms.resize(0);
    tfs.resize(0);
    return visitTermImpactFrontier([&](int32_t tf, int32_t norm) {
      tfs.push_back(tf);
      norms.push_back(norm);
    });
  }

  // Read stored impact data without decoding postings bodies.  DOCS-only fields
  // do not store impact fields, so their per-block maxTf and group span impacts are
  // synthesized as 1; fields without norms synthesize minNorm/spanMinNorm as 0.
  void readBlockMaxTf(std::vector<int32_t>& blockMaxTf,
                      std::vector<int32_t>* groupSpanImpacts = nullptr,
                      std::vector<int32_t>* blockLastDocs = nullptr,
                      std::vector<int32_t>* blockMinNorms = nullptr,
                      std::vector<int32_t>* groupSpanMinNorms = nullptr,
                      ImpactFrontiers* impactFrontiers = nullptr) const {
    blockMaxTf.resize(0);
    if (groupSpanImpacts != nullptr) {
      groupSpanImpacts->resize(0);
    }
    if (blockLastDocs != nullptr) {
      blockLastDocs->resize(0);
    }
    if (blockMinNorms != nullptr) {
      blockMinNorms->resize(0);
    }
    if (groupSpanMinNorms != nullptr) {
      groupSpanMinNorms->resize(0);
    }
    if (impactFrontiers != nullptr) {
      impactFrontiers->clear();
    }
    if (docsSize == 0) {
      return;
    }

    blockMaxTf.reserve(numDocBlocks);
    if (groupSpanImpacts != nullptr) {
      groupSpanImpacts->reserve(docGroupCount());
    }
    if (blockLastDocs != nullptr) {
      blockLastDocs->reserve(numDocBlocks);
    }
    if (blockMinNorms != nullptr) {
      blockMinNorms->reserve(numDocBlocks);
    }
    if (groupSpanMinNorms != nullptr) {
      groupSpanMinNorms->reserve(docGroupCount());
    }
    if (impactFrontiers != nullptr) {
      impactFrontiers->offsets.reserve((size_t) numDocBlocks + 1);
    }

    const char* const end = docIS.ptr(endOfDocs);
    const char* p = docIS.ptr(startOfDocs);
    uint32_t prevGroupLastDoc = 0;
    uint32_t prevBlockLastDoc = 0;

    for (int32_t group = 0; group < docGroupCount(); group++) {
      int32_t groupStartBlock = group * L1_PERIOD;
      int32_t groupBlockCount = std::min(L1_PERIOD, numDocBlocks - groupStartBlock);

      uint32_t groupHeaderLen = InputStream::readVint(p, end);
      const char* groupHeaderEnd = p + groupHeaderLen;
      assert(groupHeaderEnd <= end);
      uint32_t groupLastDoc = prevGroupLastDoc + readVint15(p, groupHeaderEnd);
      uint64_t groupByteLen = readVlong15(p, groupHeaderEnd);
      assert(p < groupHeaderEnd);
      int32_t groupPackedBlockCount = (int32_t) (uint8_t) *p++;
      assert(groupPackedBlockCount <= groupBlockCount);
      unused(groupPackedBlockCount);
      assert(p < groupHeaderEnd);
      uint8_t entryCount = (uint8_t) *p++;
      assert(p + (size_t) entryCount * sizeof(uint64_t) <= groupHeaderEnd);
      p += (size_t) entryCount * sizeof(uint64_t);
      if (hasPositions) {
        auto groupCumTfDelta = InputStream::readVint(p, groupHeaderEnd);
        unused(groupCumTfDelta);
      }
      // The group corner (max tf, min norm) is the staircase's last tf and
      // first norm for freqs+norms fields; stored directly otherwise.
      int32_t spanImpact = 1;
      int32_t spanMinNorm = 0;
      if (hasFreqs && hasNorms) {
        GroupImpactHeader packed;
        decodePackedL1Frontier(p, groupHeaderEnd, packed);
        spanMinNorm = packed.spanMinNorm;
        spanImpact = packed.spanMaxTf;
      } else if (hasFreqs) {
        spanImpact = (int32_t) InputStream::readVint(p, groupHeaderEnd);
      } else if (hasNorms) {
        spanMinNorm = (int32_t) InputStream::readVint(p, groupHeaderEnd);
      }
      assert(p == groupHeaderEnd);
      if (groupSpanImpacts != nullptr) {
        groupSpanImpacts->push_back(spanImpact);
      }
      if (groupSpanMinNorms != nullptr) {
        groupSpanMinNorms->push_back(spanMinNorm);
      }

      const char* const groupBody = groupHeaderEnd;
      const char* const groupEnd = groupBody + (int64_t) groupByteLen;
      assert(groupEnd <= end);
      p = groupBody;

      for (int32_t i = 0; i < groupBlockCount; i++) {
        uint32_t headerLen = InputStream::readVint(p, groupEnd);
        const char* headerEnd = p + headerLen;
        assert(headerEnd <= groupEnd);
        uint32_t blockLastDocValue = prevBlockLastDoc + readVint15(p, headerEnd);
        uint64_t blockByteLen = readVlong15(p, headerEnd);
        if (hasPositions) {
          auto blockCumTfDelta = InputStream::readVint(p, headerEnd);
          unused(blockCumTfDelta);
          auto blockPosByteOff = InputStream::readVlong(p, headerEnd);
          unused(blockPosByteOff);
        }
        int32_t maxTf = 1;
        int32_t minNorm = 0;
        if (impactFrontiers != nullptr) {
          impactFrontiers->offsets.push_back((int32_t) impactFrontiers->tfs.size());
        }
        if (hasFreqs && hasNorms) {
          uint32_t frontierCount = InputStream::readVint(p, headerEnd);
          assert(frontierCount > 0);
          int32_t tf = 0;
          for (uint32_t j = 0; j < frontierCount; j++) {
            assert(p < headerEnd);
            int32_t norm = (int32_t) (uint8_t) *p;  // raw byte (absolute)
            p++;
            tf += (int32_t) InputStream::readVint(p, headerEnd);  // tf delta
            assert(norm >= 0 && norm <= 255);
            assert(tf > 0);
            if (j == 0) {
              minNorm = norm;
            }
            maxTf = tf;
            if (impactFrontiers != nullptr) {
              impactFrontiers->norms.push_back(norm);
              impactFrontiers->tfs.push_back(tf);
            }
          }
        } else if (hasFreqs) {
          maxTf = (int32_t) InputStream::readVint(p, headerEnd);
        } else if (hasNorms) {
          minNorm = (int32_t) InputStream::readVint(p, headerEnd);
        }
        assert(p == headerEnd);
        blockMaxTf.push_back(maxTf);
        if (blockLastDocs != nullptr) {
          blockLastDocs->push_back((int32_t) blockLastDocValue);
        }
        if (blockMinNorms != nullptr) {
          blockMinNorms->push_back(minNorm);
        }
        p = headerEnd + (int64_t) blockByteLen;
        assert(p <= groupEnd);
        prevBlockLastDoc = blockLastDocValue;
      }

      assert(p == groupEnd);
      prevGroupLastDoc = groupLastDoc;
      assert(prevBlockLastDoc == prevGroupLastDoc);
    }

    assert((int32_t) blockMaxTf.size() == numDocBlocks);
    assert(blockMinNorms == nullptr || (int32_t) blockMinNorms->size() == numDocBlocks);
    if (impactFrontiers != nullptr) {
      impactFrontiers->offsets.push_back((int32_t) impactFrontiers->tfs.size());
      assert((int32_t) impactFrontiers->offsets.size() == numDocBlocks + 1);
      assert(impactFrontiers->tfs.size() == impactFrontiers->norms.size());
    }
    assert(p == end);
  }


  int32_t numImpactBlocks() const { return numDocBlocks; }
  int32_t numImpactGroups() const { return docGroupCount(); }

  template <typename Visitor>
  int32_t readGroupImpactHeadersThrough(GroupImpactCursor& cursor,
                                        int32_t throughGroup,
                                        bool countLazyParses,
                                        Visitor&& visitor) const {
    if (docsSize == 0 || throughGroup < 0) {
      return 0;
    }
    if (!cursor.initialized) {
      initGroupImpactCursor(cursor);
    }
    int32_t parsed = 0;
    int32_t stop = std::min(throughGroup, docGroupCount() - 1);
    while (cursor.nextGroup <= stop) {
      GroupImpactHeader header = parseNextGroupImpactHeader(cursor);
      if (countLazyParses) {
        skipCount(SkipStats::impactGroupHeaderParses);
      }
      visitor(header);
      parsed++;
    }
    if (cursor.nextGroup == docGroupCount()) {
      assert(cursor.next == docIS.ptr(endOfDocs));
    }
    return parsed;
  }

  void readGroupImpacts(GroupImpacts& out) const {
    out.lastDocs.resize(0);
    out.packedBlockCounts.resize(0);
    out.spanMaxTfs.resize(0);
    out.spanMinNorms.resize(0);
    out.bodyOffsets.resize(0);
    out.baseLastDocs.resize(0);
    out.frontiers.clear();
    if (docsSize == 0) {
      return;
    }
    out.lastDocs.reserve(docGroupCount());
    out.packedBlockCounts.reserve(docGroupCount());
    out.spanMaxTfs.reserve(docGroupCount());
    out.spanMinNorms.reserve(docGroupCount());
    out.bodyOffsets.reserve(docGroupCount());
    out.baseLastDocs.reserve(docGroupCount());
    out.frontiers.offsets.reserve((size_t) docGroupCount() + 1);

    GroupImpactCursor cursor;
    initGroupImpactCursor(cursor);
    while (cursor.nextGroup < docGroupCount()) {
      GroupImpactHeader header = parseNextGroupImpactHeader(cursor);
      out.baseLastDocs.push_back(header.baseLastDoc);
      out.lastDocs.push_back(header.lastDoc);
      out.packedBlockCounts.push_back(header.packedBlockCount);
      out.spanMaxTfs.push_back(header.spanMaxTf);
      out.spanMinNorms.push_back(header.spanMinNorm);
      out.bodyOffsets.push_back(header.bodyOffset);
      out.frontiers.offsets.push_back((int32_t) out.frontiers.tfs.size());
      for (uint32_t i = 0; i < (uint32_t) header.frontierNorms.size(); i++) {
        out.frontiers.norms.push_back((int32_t) header.frontierNorms[i]);
        out.frontiers.tfs.push_back((int32_t) readPackedL1Tf(
            header.frontierTfBytes, header.frontierTfWidth, i));
      }
    }
    assert(cursor.next == docIS.ptr(endOfDocs));
    out.frontiers.offsets.push_back((int32_t) out.frontiers.tfs.size());
  }

  template <typename Visitor>
  void visitGroupBlockImpacts(int32_t groupIndex, int64_t bodyOffset,
                              int32_t baseLastDoc,
                              GroupBlockImpactScratch& scratch,
                              Visitor&& visitor) const {
    int32_t groupStartBlock = groupIndex * L1_PERIOD;
    int32_t groupBlockCount =
        std::min(L1_PERIOD, numDocBlocks - groupStartBlock);
    assert(groupBlockCount > 0);

    const char* const end = docIS.ptr(endOfDocs);
    const char* p = docIS.ptr(startOfDocs + bodyOffset);
    uint32_t prevBlockLastDoc = (uint32_t) baseLastDoc;
    for (int32_t i = 0; i < groupBlockCount; i++) {
      uint32_t headerLen = InputStream::readVint(p, end);
      const char* headerEnd = p + headerLen;
      assert(headerEnd <= end);
      uint32_t blockLastDocValue = prevBlockLastDoc + readVint15(p, headerEnd);
      uint64_t blockByteLen = readVlong15(p, headerEnd);
      if (hasPositions) {
        auto blockCumTfDelta = InputStream::readVint(p, headerEnd);
        unused(blockCumTfDelta);
        auto blockPosByteOff = InputStream::readVlong(p, headerEnd);
        unused(blockPosByteOff);
      }
      int32_t maxTf = 1;
      int32_t minNorm = 0;
      int32_t frontierStored = 0;
      bool frontierSpilled = false;
      if (hasFreqs && hasNorms) {
        uint32_t frontierCount = InputStream::readVint(p, headerEnd);
        frontierSpilled =
            frontierCount > (uint32_t) GroupBlockImpactScratch::FRONTIER_CAP;
        if (frontierSpilled) {
          skipCount(SkipStats::impactL0GroupParseScratchSpills);
        }
        assert(frontierCount > 0);
        int32_t tf = 0;
        for (uint32_t j = 0; j < frontierCount; j++) {
          assert(p < headerEnd);
          int32_t norm = (int32_t) (uint8_t) *p++;
          tf += (int32_t) InputStream::readVint(p, headerEnd);
          if (j == 0) {
            minNorm = norm;
          }
          maxTf = tf;
          if (!frontierSpilled) {
            scratch.frontierNorms[frontierStored] = norm;
            scratch.frontierTfs[frontierStored] = tf;
            frontierStored++;
          }
        }
      } else if (hasFreqs) {
        maxTf = (int32_t) InputStream::readVint(p, headerEnd);
      } else if (hasNorms) {
        minNorm = (int32_t) InputStream::readVint(p, headerEnd);
      }
      assert(p == headerEnd);
      visitor(i, (int32_t) blockLastDocValue, maxTf, minNorm,
              std::span<const int32_t>(scratch.frontierTfs,
                                       (size_t) frontierStored),
              std::span<const int32_t>(scratch.frontierNorms,
                                       (size_t) frontierStored),
              frontierSpilled);
      p = headerEnd + (int64_t) blockByteLen;
      assert(p <= end);
      prevBlockLastDoc = blockLastDocValue;
    }
  }
};

// TODO: templatize to be able to instrument, implement checkindex, etc...
// TODO: investigate writing a version of this based on continuations and see how it performs?
// TODO: some of this internal state could be removed... we only need some of it in the constructor?
// Implementation note: moving block reading of docs and positions to cpp files and just leaving the hot path
// in the header resulted in >3% performance loss for docs, and >5% loss for positions.
class DocsEnumImpl : public DocsEnumMeta {
  friend class PosEnum;
  template<DocsEnumTier Tier>
  friend class BasicDocsEnum;

  int32_t* docBuf;       // the list of decoded docs (may be partial)
  int32_t* tfreqBuf;     // the list of decoded term freqs

  int64_t posOrdStart = 0;    // the starting position ordinal for the current doc
  int64_t cumulativeTermFreq = 0; // Synonym for posOrdEnd.  Should be equal to posOrdStart + tfreq (i.e. a docs positions are [posOrdStart,cumulativeTermFreq)
  bool positionBatchActive = false;
  bool positionTrackingEnabled = false;

  int32_t docOrd = 0;    // the ordinal of the current document we are on for this term
  int32_t docBufIdx = 0; // index of the next value to read in the docs buffer
  int32_t docBufEnd;     // index of one-past the last valid element
  int32_t docid;         // current docid
  bool blockMode = false;
  bool docsOnlyConsumed = false;
  bool docBlockResident = false;
  bool scoredProbeActive = false;
  const char* residentWordsPtr = nullptr;
  uint32_t residentDocBase = 0;
  uint32_t residentBlockLast = 0;
  int32_t residentBlockStartOrd = 0;
  int32_t residentNumWords = 0;  // zero means a contiguous resident block
  const char* scoredProbeWordsPtr = nullptr;
  uint32_t scoredProbeDocBase = 0;
  uint32_t scoredProbeBlockLast = 0;
  int32_t scoredProbeBlockStartOrd = 0;
  // Low bits encode doc representation as numWords + 1:
  // 0 packed docs in docBuf, 1 contiguous, >1 resident 64-bit words.
  // High bits carry probe-local one-shot state without growing the enum.
  uint32_t scoredProbeState = 0;

  // For term freqs, since they are parallel to docs, we don't actually need
  // all of these variables.  But it sets the stage for separating the two
  // more and using different encodings / block sizes for them.
  int32_t tfreqOrd = 0;      // ordinal of the current term freq (parallel to docOrd)
  int32_t tfreqBufIdx = 0;   // index of the next valid value
  int32_t tfreqBufEnd;       // index of one-past the last valid element
  int32_t tfreq;

  InputStream positionInput;

  int64_t posStartLoc = 0;  // absolute location of this term's positions (base for L0 posByteOff)
  int32_t pulsedPosition = 0;
  int64_t pendingPosBlockOrd = 0;
  int64_t pendingPosAbsoluteOffset = 0;
  bool pendingPosSeekValid = false;

public:
  struct DocFreqBlock {
    std::span<const int32_t> docs;
    std::span<const int32_t> tfreqs;
  };

#if SOLUX_PROBE_CONSTANT_HOOKS
  static inline bool disableSlimL0WalkForTests = false;
  static inline bool disableProbeWrapperTrimsForTests = false;
#else
  static constexpr bool disableSlimL0WalkForTests = false;
  static constexpr bool disableProbeWrapperTrimsForTests = false;
#endif

private:
  int32_t nextL0Block = 0;     // block whose header is at docIS, unless bodyReady is true
  uint32_t nextL0Base = 0;     // last doc before nextL0Block
  int64_t nextL0CumTf = 0;     // cumulative tf before nextL0Block
  int32_t nextL1Group = 0;     // group whose header is next when nextL0Block is on a group boundary
  uint32_t nextL1Base = 0;     // last doc before nextL1Group
  uint32_t readyBlockBodyBytes = 0;  // body length retained by an L0 cursor landing
  bool bodyReady = false;      // docIS already points at the current block body after an L0 walk
  const char* l0CheckpointEntries = nullptr;
  const char* l0CheckpointGroupBody = nullptr;
  int32_t l0CheckpointGroupStartBlock = -1;
  uint8_t l0CheckpointCount = 0;

  // Position metadata repair can be deferred while a positions-tracking enum is
  // only being advanced through decoded docs. The dirty range is always within
  // the current decoded doc/freq block; block skips and block-boundary decodes
  // re-anchor cumulativeTermFreq from L0 metadata and clear it.
  bool posRepairDirty = false;
  int32_t posRepairStart = 0;  // inclusive index in tfreqBuf/docBuf
  int32_t posRepairEnd = 0;    // exclusive index; current doc is end - 1

  // +8: expandDocWords writes branchless 8-wide rows; if the last byte of the
  // last word has popcount 0 (common - it just means the block's final doc
  // isn't in that byte's bit range), the unconditional dst[0..7] write for
  // that byte lands entirely past the true end, needing indices up to
  // DOCS_BLOCK_SIZE + 7.
  int32_t db[Postings::DOCS_BLOCK_SIZE + 8];  // temporary...
  int32_t tb[Postings::POSITIONS_BLOCK_SIZE];

  static bool isL1Boundary(int32_t block) {
    return (block % L1_PERIOD) == 0;
  }

  static constexpr uint32_t SCORED_PROBE_REP_MASK = 0xffff;
  static constexpr uint32_t SCORED_PROBE_FREQS_DECODED = 1u << 30;
  static constexpr uint32_t SCORED_PROBE_SURVIVOR_COUNTED = 1u << 31;

  int32_t scoredProbeNumWords() const {
    return (int32_t) (scoredProbeState & SCORED_PROBE_REP_MASK) - 1;
  }

  bool scoredProbeFreqsDecoded() const {
    return (scoredProbeState & SCORED_PROBE_FREQS_DECODED) != 0;
  }

  static uint64_t lowBitsMask(int32_t bits) {
    assert(bits >= 0 && bits <= 64);
    if (bits == 0) {
      return 0;
    }
    if (bits == 64) {
      return ~0ULL;
    }
    return (1ULL << bits) - 1ULL;
  }

  static uint64_t loadWord64(const char* p) {
    uint64_t word;
    memcpy(&word, p, 8);
    return word;
  }

  // Position docIS at the current block body: skip the group header on an L1
  // boundary and the L0 header, unless an L0 walk already left the stream at
  // the body (bodyReady).
  void seekToBlockBody() {
    if (!bodyReady) {
      if (isL1Boundary(nextL0Block)) {
        auto groupHeaderLen = docIS.readVint();
        docIS.skip(groupHeaderLen);
      }
      auto headerLen = docIS.readVint();
      docIS.skip(headerLen);
    } else {
      bodyReady = false;
      readyBlockBodyBytes = 0;
    }
  }

  uint32_t seekToScoredProbeBlockBody() {
    if (bodyReady) {
      bodyReady = false;
      uint32_t bodyBytes = readyBlockBodyBytes;
      readyBlockBodyBytes = 0;
      assert(bodyBytes > 0);
      return bodyBytes;
    }
    if (isL1Boundary(nextL0Block)) {
      auto groupHeaderLen = docIS.readVint();
      docIS.skip(groupHeaderLen);
    }
    auto headerLen = docIS.readVint();
    const char* p = docIS.ptr();
    const char* headerEnd = p + headerLen;
    unused(readVint15(p, headerEnd));
    uint64_t blockByteLen = readVlong15(p, headerEnd);
    assert(blockByteLen <= (uint64_t) std::numeric_limits<uint32_t>::max());
    docIS.skip(headerLen);
    return (uint32_t) blockByteLen;
  }

  // BITPOS[b][i] = bit index of the i-th set bit of byte b (0 in unused slots).
  static constexpr auto BITPOS = [] {
    std::array<std::array<uint8_t, 8>, 256> t{};
    for (int b = 0; b < 256; b++) {
      int n = 0;
      for (int i = 0; i < 8; i++) {
        if (b & (1 << i)) {
          t[(size_t) b][(size_t) n++] = (uint8_t) i;
        }
      }
    }
    return t;
  }();

  // Expand a bitset-encoded doc block (numWords 64-bit words over
  // [docBase, lastDoc], see Postings::DOC_BLOCK_*) into docBuf. Exactly
  // DOCS_BLOCK_SIZE bits are set. Each byte emits a branchless 8-wide row
  // (vectorized u8->i32 widen + add); dst advances by the byte's popcount, so
  // up to 8 slots past the final doc are scribbled (docBuf is padded for it).
  void expandDocWords(const char* p, int32_t numWords, uint32_t docBase) {
    int32_t* dst = docBuf;
    for (int32_t w = 0; w < numWords; w++) {
      uint64_t word;
      memcpy(&word, p + (int64_t) w * 8, 8);
      const uint32_t wordBase = docBase + (uint32_t) (w << 6);
      for (int32_t b = 0; b < 8; b++) {
        const uint8_t byte = (uint8_t) (word >> (b * 8));
        const uint32_t byteBase = wordBase + (uint32_t) (b << 3);
        const uint8_t* row = BITPOS[byte].data();
        for (int32_t i = 0; i < 8; i++) {
          dst[i] = (int32_t) (byteBase + row[i]);
        }
        dst += std::popcount(byte);
      }
    }
    assert(dst - docBuf == Postings::DOCS_BLOCK_SIZE);
  }

  void clearDocBlockResident() {
    docBlockResident = false;
    residentWordsPtr = nullptr;
    residentDocBase = 0;
    residentBlockLast = 0;
    residentBlockStartOrd = 0;
    residentNumWords = 0;
  }

  void clearScoredProbe() {
    scoredProbeActive = false;
    scoredProbeWordsPtr = nullptr;
    scoredProbeDocBase = 0;
    scoredProbeBlockLast = 0;
    scoredProbeBlockStartOrd = 0;
    scoredProbeState = 0;
  }

  uint64_t residentWord(int32_t wordIndex) const {
    assert(docBlockResident);
    assert(residentNumWords > 0);
    assert(wordIndex >= 0 && wordIndex < residentNumWords);
    return loadWord64(residentWordsPtr + (int64_t) wordIndex * 8);
  }

  int32_t residentBlockEndOrd() const {
    assert(docBlockResident);
    return residentBlockStartOrd + Postings::DOCS_BLOCK_SIZE;
  }

  int32_t residentOrdinalAfterDoc(int32_t doc) const {
    assert(docBlockResident);
    assert(doc >= (int32_t) residentDocBase && doc <= (int32_t) residentBlockLast);
    if (residentNumWords == 0) {
      return residentBlockStartOrd + doc - (int32_t) residentDocBase + 1;
    }

    const int32_t bitIndex = doc - (int32_t) residentDocBase;
    const int32_t wordIndex = bitIndex >> 6;
    const int32_t bit = bitIndex & 63;
    int32_t ordInBlock = 0;
    for (int32_t w = 0; w < wordIndex; w++) {
      ordInBlock += (int32_t) std::popcount(residentWord(w));
    }
    const uint64_t word = residentWord(wordIndex);
    assert((word & (1ULL << bit)) != 0);
    ordInBlock += (int32_t) std::popcount(word & lowBitsMask(bit)) + 1;
    return residentBlockStartOrd + ordInBlock;
  }

  bool findResidentGEQ(int32_t target, int32_t& landing, int32_t& ordAfter) const {
    assert(docBlockResident);
    if (target > (int32_t) residentBlockLast) {
      return false;
    }
    if (residentNumWords == 0) {
      landing = std::max(target, (int32_t) residentDocBase);
      if (landing > (int32_t) residentBlockLast) {
        return false;
      }
      ordAfter = residentBlockStartOrd + landing - (int32_t) residentDocBase + 1;
      return true;
    }

    int32_t bitIndex = target - (int32_t) residentDocBase;
    if (bitIndex < 0) {
      bitIndex = 0;
    }
    int32_t startWord = bitIndex >> 6;
    int32_t startBit = bitIndex & 63;
    if (startWord >= residentNumWords) {
      return false;
    }

    int32_t prefix = 0;
    for (int32_t w = 0; w < residentNumWords; w++) {
      const uint64_t word = residentWord(w);
      if (w < startWord) {
        prefix += (int32_t) std::popcount(word);
        continue;
      }

      uint64_t probe = word;
      if (w == startWord) {
        probe &= ~lowBitsMask(startBit);
      }
      if (probe != 0) {
        const int32_t bit = (int32_t) std::countr_zero(probe);
        landing = (int32_t) residentDocBase + (w << 6) + bit;
        ordAfter = residentBlockStartOrd + prefix
                   + (int32_t) std::popcount(word & lowBitsMask(bit)) + 1;
        return true;
      }
      prefix += (int32_t) std::popcount(word);
    }
    return false;
  }

  bool findResidentLastBefore(int32_t upTo, int32_t minDoc,
                              int32_t& lastDoc, int32_t& ordAfter) const {
    assert(docBlockResident);
    if (upTo <= minDoc || minDoc > (int32_t) residentBlockLast) {
      return false;
    }
    if (residentNumWords == 0) {
      lastDoc = std::min((int32_t) residentBlockLast, upTo - 1);
      if (lastDoc < std::max(minDoc, (int32_t) residentDocBase)) {
        return false;
      }
      ordAfter = residentBlockStartOrd + lastDoc - (int32_t) residentDocBase + 1;
      return true;
    }

    int32_t startBit = minDoc - (int32_t) residentDocBase;
    if (startBit < 0) {
      startBit = 0;
    }
    int32_t endBit = upTo - (int32_t) residentDocBase;
    if (endBit <= startBit) {
      return false;
    }
    const int32_t maxBits = residentNumWords << 6;
    if (endBit > maxBits) {
      endBit = maxBits;
    }

    int32_t found = -1;
    const int32_t firstWord = startBit >> 6;
    const int32_t lastWord = (endBit - 1) >> 6;
    for (int32_t w = firstWord; w <= lastWord && w < residentNumWords; w++) {
      uint64_t word = residentWord(w);
      const int32_t lo = w == firstWord ? (startBit & 63) : 0;
      const int32_t hi = w == lastWord ? ((endBit - 1) & 63) + 1 : 64;
      word &= lowBitsMask(hi) & ~lowBitsMask(lo);
      if (word != 0) {
        found = (w << 6) + (63 - (int32_t) std::countl_zero(word));
      }
    }
    if (found < 0) {
      return false;
    }
    lastDoc = (int32_t) residentDocBase + found;
    ordAfter = residentOrdinalAfterDoc(lastDoc);
    return true;
  }

  void positionDocOnlyResidentAt(int32_t landing, int32_t ordAfter, bool asBlockMode) {
    assert(docBlockResident);
    assert(ordAfter >= residentBlockStartOrd + 1);
    assert(ordAfter <= residentBlockEndOrd());
    docid = landing;
    docOrd = ordAfter;
    docBufIdx = ordAfter - residentBlockStartOrd;
    docBufEnd = Postings::DOCS_BLOCK_SIZE;
    if (hasFreqs) {
      tfreqOrd = ordAfter;
      tfreqBufIdx = docBufIdx;
      tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
    }
    tfreq = 1;
    blockMode = asBlockMode;
  }

  void finishDocOnlyResidentBlock() {
    assert(docBlockResident);
    docid = (int32_t) residentBlockLast;
    docOrd = residentBlockEndOrd();
    docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
    if (hasFreqs) {
      tfreqOrd = docOrd;
      tfreqBufIdx = tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
    }
    tfreq = 1;
    blockMode = true;
    docBuf[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) residentBlockLast;
    clearDocBlockResident();
  }

  void materializeDocOnlyResidentBlock() {
    if (!docBlockResident) {
      return;
    }
    const int32_t idx = docOrd - residentBlockStartOrd;
    const uint32_t docBase = residentDocBase;
    const uint32_t blockLast = residentBlockLast;
    const int32_t numWords = residentNumWords;
    const char* wordsPtr = residentWordsPtr;
    if (numWords == 0) {
      for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
        docBuf[i] = (int32_t) docBase + i;
      }
    } else {
      expandDocWords(wordsPtr, numWords, docBase);
    }
    assert(docBuf[Postings::DOCS_BLOCK_SIZE - 1] == (int32_t) blockLast);
    clearDocBlockResident();
    docBufIdx = idx;
    docBufEnd = Postings::DOCS_BLOCK_SIZE;
  }

  uint32_t decodeScoredProbeFreqs(const char* freqPtr, uint32_t bytesAvailable) {
    assert(scoredProbeActive);
    assert(hasFreqs);
    assert(!scoredProbeFreqsDecoded());
    uint32_t outSz = Postings::DOCS_BLOCK_SIZE;
    auto bytesRead = IndexCodec::tfreqCodec.decodeBlock(
        freqPtr, bytesAvailable, (uint32_t*) tfreqBuf, outSz);
    assert(outSz == Postings::DOCS_BLOCK_SIZE);
    scoredProbeState |= SCORED_PROBE_FREQS_DECODED;
    tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
    skipCount(SkipStats::tfreqBlocksDecoded);
    skipCount(SkipStats::scoredProbeFreqDecodes);
    return bytesRead;
  }

  void ensureScoredProbeFreqsDecoded() {
    assert(scoredProbeActive);
    if (!hasFreqs || scoredProbeFreqsDecoded()) {
      return;
    }
    const int32_t encodedBytes = tfreqBufEnd;
    assert(encodedBytes > 0);
    const char* freqPtr = docIS.ptr() - encodedBytes;
    uint32_t bytesRead = decodeScoredProbeFreqs(
        freqPtr, (uint32_t) encodedBytes);
    assert(bytesRead == (uint32_t) encodedBytes);
  }

  void materializeScoredProbeBlock() {
    assert(scoredProbeActive);
    const int32_t idx = docOrd - scoredProbeBlockStartOrd - 1;
    assert(idx >= 0 && idx < Postings::DOCS_BLOCK_SIZE);
    ensureScoredProbeFreqsDecoded();
    const int32_t numWords = scoredProbeNumWords();
    if (numWords == 0) {
      for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
        docBuf[i] = (int32_t) scoredProbeDocBase + i;
      }
    } else if (numWords > 0) {
      expandDocWords(scoredProbeWordsPtr, numWords, scoredProbeDocBase);
      skipCount(SkipStats::scoredProbeWordExpansions);
    }
    assert(docBuf[Postings::DOCS_BLOCK_SIZE - 1] == (int32_t) scoredProbeBlockLast);
    if (!hasFreqs) {
      std::fill(tfreqBuf, tfreqBuf + Postings::DOCS_BLOCK_SIZE, 1);
    }
    docBufIdx = idx + 1;
    docBufEnd = Postings::DOCS_BLOCK_SIZE;
    tfreqOrd = docOrd;
    tfreqBufIdx = idx + 1;
    tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
    tfreq = hasFreqs ? tfreqBuf[idx] : 1;
    blockMode = false;
    clearScoredProbe();
  }

  void promoteDecodedPackedScoredProbe() {
    assert(scoredProbeActive);
    assert(scoredProbeNumWords() < 0);
    assert(!hasFreqs || scoredProbeFreqsDecoded());
    materializeScoredProbeBlock();
  }

  int32_t advanceScoredNoPositionsFromReadyBlock(int32_t target) {
    assert(docid < target);
    assert(!positionTrackingEnabled);
    assert(!docsOnlyConsumed);
    assert(!docBlockResident);
    assert(!scoredProbeActive);
    if (docBufIdx >= docBufEnd) {
      if (nextDocImpl<false>() >= target) {
        return docid;
      }
    }
    if (docid == PostingsReader::END) {
      return docid;
    }
    if (docBuf[docBufEnd - 1] < target) {
      while (docid < target) {
        nextDocImpl<false>();
      }
      return docid;
    }

    blockMode = false;
    const int32_t start = docBufIdx;
    const int32_t j =
        DecodedSuccessor::index(docBuf, start, docBufEnd, target);
    assert(j < docBufEnd);
    const int32_t consumed = j + 1 - start;
    docOrd += consumed;
    if (hasFreqs) {
      tfreq = tfreqBuf[j];
      tfreqOrd += consumed;
      tfreqBufIdx = j + 1;
    } else {
      tfreq = 1;
    }
    docBufIdx = j + 1;
    docid = docBuf[j];
    return docid;
  }

  // Called once by PosEnum's constructor to switch on the alignment
  // bookkeeping (cumulativeTermFreq/posOrdStart/deferred repair) that the
  // doc/freq decode hot path otherwise skips entirely.
  void enablePositionTracking() {
    assert(!scoredProbeActive);
    positionTrackingEnabled = true;
  }

  void clearPendingPositionRepair() {
    posRepairDirty = false;
    posRepairStart = posRepairEnd = 0;
  }

  void markPendingPositionRepair(int32_t start, int32_t end) {
    if (!positionTrackingEnabled || start >= end) {
      return;
    }
    if (posRepairDirty) {
      assert(start == posRepairEnd);
      posRepairEnd = end;
    } else {
      posRepairDirty = true;
      posRepairStart = start;
      posRepairEnd = end;
    }
  }

  void materializePendingPositionRepair() {
    if (!posRepairDirty) {
      return;
    }
    int64_t sum = 0;
    if (hasFreqs) {
      for (int32_t i = posRepairStart; i < posRepairEnd; i++) {
        sum += (uint32_t) tfreqBuf[i];
      }
    } else {
      sum = posRepairEnd - posRepairStart;
    }
    cumulativeTermFreq += sum;
    posOrdStart = cumulativeTermFreq - tfreq;
    clearPendingPositionRepair();
  }

  void discardPendingPositionRepairAtBlockBoundary() {
    if (!posRepairDirty) {
      return;
    }
    if (docsSize == 0) {
      // Pulsed postings have no block header to re-anchor from; the dirty range
      // is one doc, so materializing is still cheap.
      materializePendingPositionRepair();
      return;
    }
    cumulativeTermFreq = nextL0CumTf;
    clearPendingPositionRepair();
  }

public:
protected:
  explicit DocsEnumImpl(const TermsEnum::PostingsState& state)
  : DocsEnumMeta(state), positionInput(state.posIS),
    posStartLoc(state.posStart), pulsedPosition(state.pulsedPos)
  {
    docBuf=db;
    tfreqBuf=tb;
    docid = -1;

    if (docsSize == 0) {
      // postings pulsed
      assert(docfreq == 1);
      tfreq = 1;
      assert(ttf == 1);
      // Fill the doc/freq buffers. PosEnum initializes the pulsed position lazily.
      docBuf[0] = state.pulsedDoc;
      docBufEnd = 1;
      tfreqBuf[0] = 1;
      tfreqBufEnd = 1;
      cumulativeTermFreq = 0;  // this will be incremented in nextDoc()

    } else {
      tfreq = -1;  // unnecessary initialization, but it avoids a maybe-uninitialized warning with -O3
      assert(endOfDocs - startOfDocs == docsSize);
      docBufEnd = 0;

      if (hasPositions) {
        assert(positionInput.offset() == posStartLoc);
      }

      assert(docIS.offset() == startOfDocs);

      cumulativeTermFreq = 0;
      nextL0Block = 0;
      nextL0Base = 0;
      nextL0CumTf = 0;
      nextL1Group = 0;
      nextL1Base = 0;
      readyBlockBodyBytes = 0;
      bodyReady = false;
      // std::cout << "Normal posting" << std::endl;
    }
  }

  explicit DocsEnumImpl(TermsEnum& termsEnum)
      : DocsEnumImpl(termsEnum.postingsState()) {}

  DocsEnumImpl(const DocsEnumImpl& other) = delete;

public:
  /// the document this iterator is currently positions on
  int32_t docId() {
    return docid;
  }

  /// number of times the term appears in the current document
  int32_t termFreq() {
    assert(!docBlockResident);
    assert(!docsOnlyConsumed);
    if (scoredProbeActive) {
      if ((scoredProbeState & SCORED_PROBE_SURVIVOR_COUNTED) == 0) {
        scoredProbeState |= SCORED_PROBE_SURVIVOR_COUNTED;
        skipCount(SkipStats::scoredProbeSurvivorBlocks);
      }
      if (!scoredProbeFreqsDecoded()) {
        ensureScoredProbeFreqsDecoded();
        const int32_t idx = docOrd - scoredProbeBlockStartOrd - 1;
        assert(idx >= 0 && idx < Postings::DOCS_BLOCK_SIZE);
        tfreq = hasFreqs ? tfreqBuf[idx] : 1;
      }
      // Packed docs already live in docBuf. Once a survivor forces the freq
      // plane resident too, this is an ordinary decoded block: publish that
      // cursor immediately so later high-survival probes use the compact
      // decoded-remainder loop without representation or lazy-state checks.
      if (scoredProbeNumWords() < 0) {
        promoteDecodedPackedScoredProbe();
      }
    }
    return tfreq;
  }

  // we also have a next() to align with scorers
  int32_t next() {
    return nextDoc();
  }

private:
  // Shared block-decode walk behind nextDoc()/nextDocOnly(). DOCS_ONLY skips
  // freq blocks undecoded via the codec (StreamVByte tails carry no length
  // prefix, so tail freqs still decode), pins tfreq to 1, and maintains no
  // position/cumulative-tf state (a docs-only enum can never serve positions).
  template <bool DOCS_ONLY>
  int32_t nextDocImpl() {
    // Contract: callers must not re-poll after END (see Query::Scorer).
    assert(docid != PostingsReader::END);
    assert(!docBlockResident);
    assert(!positionBatchActive);
    blockMode = false;
    if (docBufIdx >= docBufEnd) {
      if constexpr (!DOCS_ONLY) {
        discardPendingPositionRepairAtBlockBoundary();
      }
      auto leftToRead = docfreq - docOrd;
      // Boundary analysis: if docfreq==1 and docOrd==1 (meaning we already read ord 0, but not 1), we are done.
      if (leftToRead <= 0) {
        assert(leftToRead == 0);
        docid = PostingsReader::END;
        return docid;
      }

      // Cross-block delta base: the previous block's last id (still in docBuf,
      // not yet overwritten), or 0 for the first block.  Mirrors PostingsWriter
      // (full blocks via the docs codec, the partial tail via StreamVByte d1).
      const uint32_t base = (docOrd == 0) ? 0 : (uint32_t) docBuf[Postings::DOCS_BLOCK_SIZE - 1];
      const int32_t blockStartOrd = docOrd;

      seekToBlockBody();
      // Every path from here decodes exactly one docs block (full block or tail).
      skipCount(SkipStats::docBlocksDecoded);

      // Since we only read whole blocks, simply comparing with number of docs left to read is sufficient.
      // If we start partial decoding of blocks (say because of skipping), then we would want something
      // like lastBlockEncodedPosOrd, but for docs.
      if (leftToRead >= Postings::DOCS_BLOCK_SIZE) {
        const int8_t token = (int8_t) *docIS.ptr();
        docIS.skip(1);
        if (token > 0) {
          uint32_t outSz = Postings::DOCS_BLOCK_SIZE;
          auto bytesRead = IndexCodec::docCodec.decodeBlock(docIS.ptr(), docIS.left(), (uint32_t*)docBuf, outSz, base);
          docIS.skip(bytesRead);
          assert(outSz == Postings::DOCS_BLOCK_SIZE);
        } else {
          // docBase = doc id of bit 0: the term's first block spans from the
          // (zero) base itself, later blocks from one past the previous
          // block's last id (see PostingsWriter::flushDocs).
          const uint32_t docBase = base + (blockStartOrd == 0 ? 0 : 1);
          if (token == Postings::DOC_BLOCK_CONTIGUOUS) {
            for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
              docBuf[i] = (int32_t) docBase + i;
            }
          } else {
            expandDocWords(docIS.ptr(), -token, docBase);
            docIS.skip((int64_t) -token * 8);
          }
        }
        docBufIdx = 0;
        docBufEnd = Postings::DOCS_BLOCK_SIZE;

        // Term freqs are omitted entirely for DOCS-only fields; tfreq is then implicitly 1.
        if (hasFreqs) {
          if constexpr (DOCS_ONLY) {
            auto bytesSkipped = IndexCodec::tfreqCodec.skipBlock(docIS.ptr(), docIS.left());
            docIS.skip(bytesSkipped);
            skipCount(SkipStats::docsOnlyFreqBlocksSkipped);
          } else {
            uint32_t outSz = Postings::DOCS_BLOCK_SIZE;  // currently parallel to docs, so must be same block size
            auto bytesRead = IndexCodec::tfreqCodec.decodeBlock(docIS.ptr(), docIS.left(), (uint32_t*)tfreqBuf, outSz);
            docIS.skip(bytesRead);
            assert(outSz == Postings::DOCS_BLOCK_SIZE);
            skipCount(SkipStats::tfreqBlocksDecoded);
          }
          tfreqBufIdx = 0;
          tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
        }
        if constexpr (!DOCS_ONLY) {
          if (positionTrackingEnabled) {
            // cumulativeTermFreq still sits at the block start here: the
            // boundary discard above re-anchored it, and only the per-doc
            // advance below moves it. Constant loop bound: this sum is on the
            // phrase-decode hot path.
            int64_t blockTfSum = 0;
            for (int i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
              blockTfSum += tfreqBuf[i];
            }
            nextL0CumTf = cumulativeTermFreq + blockTfSum;
          }
        }
      } else {
        // StreamVByte tail layout written by PostingsWriter::endTerm:
        //   [docKeys][docData] followed, for fields with freqs, by [tfreqKeys][tfreqData].
        // Docs are d1 decoded; freqs are plain StreamVByte values with no length
        // prefix, so even docs-only consumption decodes them. The AVX decoders
        // may read up to SVB_OVERREAD_PAD bytes past the encoded data, covered by
        // the trailing padding PostingsWriter::finish() reserves on the docs file.
        const uint32_t n = (uint32_t) leftToRead;
        const uint32_t kb = svbKeyBytes(n);
        uint8_t* p = (uint8_t*) docIS.ptr();
        uint8_t* dataEnd = svb_decode_avx_d1_init((uint32_t*) docBuf, p, p + kb, n, base);
        if (hasFreqs) {
          dataEnd = svb_decode_avx_simple((uint32_t*) tfreqBuf, dataEnd, dataEnd + kb, n);
          tfreqBufIdx = 0;
          tfreqBufEnd = leftToRead;
        }
        docIS.skip(dataEnd - p);

        docBufIdx = 0;
        docBufEnd = leftToRead;
        if constexpr (!DOCS_ONLY) {
          if (positionTrackingEnabled) {
            int64_t blockTfSum = 0;
            for (int i = 0; i < leftToRead; i++) {
              blockTfSum += tfreqBuf[i];
            }
            nextL0CumTf = cumulativeTermFreq + blockTfSum;
          }
        }
      }

      nextL0Base = (uint32_t) docBuf[docBufEnd - 1];
      nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
      if (isL1Boundary(nextL0Block)) {
        nextL1Group = nextL0Block / L1_PERIOD;
        nextL1Base = nextL0Base;
      }
    }

    // Keep tfreq current for scoring; cumulativeTermFreq is only needed by
    // position consumers such as PhraseQuery.
    assert(!hasFreqs || docBufIdx == tfreqBufIdx);
    assert(!hasFreqs || docOrd == tfreqOrd);

    docid = docBuf[docBufIdx++];
    docOrd++;

    if constexpr (DOCS_ONLY) {
      if (hasFreqs) {
        tfreqBufIdx = docBufIdx;
        tfreqOrd++;
      }
      tfreq = 1;
    } else {
      // DOCS-only fields have no freq stream; tfreq is implicitly 1.
      if (hasFreqs) {
        tfreq = tfreqBuf[tfreqBufIdx++];
        tfreqOrd++;
      } else {
        tfreq = 1;
      }
      if (positionTrackingEnabled) {
        markPendingPositionRepair(docBufIdx - 1, docBufIdx);
      }
    }

    return docid;
  }

public:
  int32_t nextDoc() {
    assert(!docBlockResident);
    assert(!docsOnlyConsumed);
    if (scoredProbeActive) {
      materializeScoredProbeBlock();
    }
    return nextDocImpl<false>();
  }

  // Docs-only consumption: freq blocks are skipped without decoding and tfreq
  // is pinned to 1. One-way: after the first nextDocOnly() this enum can never
  // serve termFreq() or positions again.
  int32_t nextDocOnly() {
    assert(!scoredProbeActive);
    docsOnlyConsumed = true;
    if (docBlockResident) {
      int32_t landing = 0;
      int32_t ordAfter = 0;
      if (findResidentGEQ(docid + 1, landing, ordAfter)) {
        positionDocOnlyResidentAt(landing, ordAfter, false);
        return docid;
      }
      finishDocOnlyResidentBlock();
    }
    return nextDocImpl<true>();
  }

  std::span<const int32_t> peekDocOnlyBlock() {
    assert(!scoredProbeActive);
    // One-way docs-only consumption, like nextDocOnly(): a peeked block may be
    // materialized from resident words with freqs skipped, so termFreq() and
    // positions are off the table from here on.
    docsOnlyConsumed = true;
    if (docid == PostingsReader::END) {
      return {};
    }
    materializeDocOnlyResidentBlock();

    int32_t start = 0;
    if (!blockMode) {
      if (docid < 0) {
        int32_t doc = nextDocOnly();
        if (doc == PostingsReader::END) {
          return {};
        }
      }
      start = docBufIdx - 1;
    } else {
      if (docBufIdx >= docBufEnd) {
        int32_t doc = nextDocOnly();
        if (doc == PostingsReader::END) {
          return {};
        }
        start = docBufIdx - 1;
      } else {
        start = docBufIdx;
      }
    }

    if (start >= docBufEnd) {
      return {};
    }

    int32_t emitted = docBufEnd - start;
    return std::span<const int32_t>(docBuf + start, (size_t) emitted);
  }

  void consumeDocOnlyBlock(int32_t n) {
    assert(n >= 0);
    assert(!scoredProbeActive);
    if (n == 0) {
      return;
    }
    assert(docsOnlyConsumed);
    assert(docid != PostingsReader::END);
    materializeDocOnlyResidentBlock();

    int32_t start = 0;
    bool countedCurrent = false;
    if (!blockMode) {
      assert(docid >= 0);
      start = docBufIdx - 1;
      countedCurrent = true;
    } else {
      start = docBufIdx;
    }

    int32_t limit = start + n;
    assert(start >= 0);
    assert(limit <= docBufEnd);

    int32_t newlyCounted = n - (countedCurrent ? 1 : 0);
    assert(newlyCounted >= 0);
    docOrd += newlyCounted;
    if (hasFreqs) {
      tfreqOrd += newlyCounted;
      tfreqBufIdx = limit;
    }
    docBufIdx = limit;
    tfreq = 1;
    docid = docBuf[limit - 1];
    blockMode = true;
  }

private:
  // OR decoded doc ids into `bits` (bit = doc - bitsBase); a contiguous run
  // collapses to a word-mask range fill.
  static void orDocBits(std::span<uint64_t> bits, const int32_t* docs,
                        int32_t count, int32_t bitsBase) {
    if (count <= 0) {
      return;
    }
    if (docs[count - 1] - docs[0] == count - 1) {
      skipCount(SkipStats::countBulkFillContiguousBlocks);
      orBitRange(bits, docs[0] - bitsBase, count);
      return;
    }
    for (int32_t i = 0; i < count; i++) {
      int32_t index = docs[i] - bitsBase;
      bits[(size_t) (index >> 6)] |= 1ULL << (index & 63);
    }
  }

  static void orBitRange(std::span<uint64_t> bits, int32_t firstIndex,
                         int32_t count) {
    assert(firstIndex >= 0);
    assert(count >= 0);
    int32_t word = firstIndex >> 6;
    int32_t bit = firstIndex & 63;
    while (count > 0) {
      int32_t take = std::min(count, 64 - bit);
      uint64_t mask = take == 64 ? ~0ULL : ((1ULL << take) - 1ULL) << bit;
      bits[(size_t) word] |= mask;
      count -= take;
      word++;
      bit = 0;
    }
  }

  // OR stored bitset words (bit i = doc docBase + i) into `bits`
  // (bit j = doc bitsBase + j), recording only docs >= clipDoc. Requires
  // clipDoc >= bitsBase; the caller guarantees every recorded bit lands
  // inside `bits`.
  static void orShiftedWords(std::span<uint64_t> bits, int32_t bitsBase,
                             const char* src, int32_t numWords,
                             uint32_t docBase, int32_t clipDoc) {
    assert(clipDoc >= bitsBase);
    for (int32_t w = 0; w < numWords; w++) {
      uint64_t word;
      memcpy(&word, src + (int64_t) w * 8, 8);
      if (word == 0) {
        continue;
      }
      const int64_t wordDoc0 = (int64_t) docBase + ((int64_t) w << 6);
      if (wordDoc0 + 63 < clipDoc) {
        continue;
      }
      if (clipDoc > wordDoc0) {
        word &= ~0ULL << (clipDoc - wordDoc0);
        if (word == 0) {
          continue;
        }
      }
      int64_t dstBit0 = wordDoc0 - bitsBase;
      if (dstBit0 < 0) {
        // Bits below bitsBase were cleared by the clip (clipDoc >= bitsBase),
        // so the surviving bits shift into range.
        word >>= (uint32_t) -dstBit0;
        dstBit0 = 0;
      }
      const size_t idx = (size_t) (dstBit0 >> 6);
      const int32_t off = (int32_t) (dstBit0 & 63);
      bits[idx] |= word << off;
      if (off != 0) {
        uint64_t hi = word >> (64 - off);
        if (hi != 0) {
          bits[idx + 1] |= hi;
        }
      }
    }
  }

  static void orShiftedWordsRange(std::span<uint64_t> bits, int32_t bitsBase,
                                  const char* src, int32_t numWords,
                                  uint32_t docBase, int32_t clipDoc,
                                  int32_t upTo) {
    assert(clipDoc >= bitsBase);
    assert(upTo >= clipDoc);
    for (int32_t w = 0; w < numWords; w++) {
      uint64_t word;
      memcpy(&word, src + (int64_t) w * 8, 8);
      if (word == 0) {
        continue;
      }
      const int64_t wordDoc0 = (int64_t) docBase + ((int64_t) w << 6);
      if (wordDoc0 + 63 < clipDoc) {
        continue;
      }
      if (wordDoc0 >= upTo) {
        break;
      }
      int32_t lo = 0;
      int32_t hi = 64;
      if (clipDoc > wordDoc0) {
        lo = clipDoc - (int32_t) wordDoc0;
      }
      if ((int64_t) upTo < wordDoc0 + 64) {
        hi = upTo - (int32_t) wordDoc0;
      }
      if (lo >= hi) {
        continue;
      }
      word &= lowBitsMask(hi) & ~lowBitsMask(lo);
      if (word == 0) {
        continue;
      }
      int64_t dstBit0 = wordDoc0 - bitsBase;
      if (dstBit0 < 0) {
        word >>= (uint32_t) -dstBit0;
        dstBit0 = 0;
      }
      const size_t idx = (size_t) (dstBit0 >> 6);
      const int32_t off = (int32_t) (dstBit0 & 63);
      bits[idx] |= word << off;
      if (off != 0) {
        uint64_t hiWord = word >> (64 - off);
        if (hiWord != 0) {
          bits[idx + 1] |= hiWord;
        }
      }
    }
  }

  bool advanceDocOnlyResident(int32_t target) {
    assert(docBlockResident);
    int32_t landing = 0;
    int32_t ordAfter = 0;
    if (!findResidentGEQ(target, landing, ordAfter)) {
      finishDocOnlyResidentBlock();
      return false;
    }
    positionDocOnlyResidentAt(landing, ordAfter, false);
    skipCount(SkipStats::docsOnlyWordProbeAdvances);
    return true;
  }

  void enterDocOnlyResidentBlock(int32_t blockStartOrd, uint32_t docBase,
                                 uint32_t blockLast, int32_t numWords) {
    assert(!docBlockResident);
    assert(!positionTrackingEnabled);
    assert(bodyReady);
    assert(docIS.ptr() < docIS.ptr(endOfDocs));

    bodyReady = false;
    readyBlockBodyBytes = 0;
    docIS.skip(1);
    const char* wordsPtr = numWords == 0 ? nullptr : docIS.ptr();
    docIS.skip((int64_t) numWords * 8);

    if (hasFreqs) {
      auto bytesSkipped = IndexCodec::tfreqCodec.skipBlock(docIS.ptr(), docIS.left());
      docIS.skip(bytesSkipped);
      skipCount(SkipStats::docsOnlyFreqBlocksSkipped);
    }

    docBlockResident = true;
    residentWordsPtr = wordsPtr;
    residentDocBase = docBase;
    residentBlockLast = blockLast;
    residentBlockStartOrd = blockStartOrd;
    residentNumWords = numWords;

    docBuf[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) blockLast;
    docBufEnd = Postings::DOCS_BLOCK_SIZE;
    nextL0Base = blockLast;
    nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
    if (isL1Boundary(nextL0Block)) {
      nextL1Group = nextL0Block / L1_PERIOD;
      nextL1Base = nextL0Base;
    }
  }

  bool tryEnterDocOnlyResidentBlock(int32_t target) {
    assert(!docBlockResident);
    assert(!positionTrackingEnabled);
    if (docfreq - docOrd < Postings::DOCS_BLOCK_SIZE) {
      return false;
    }

    const int32_t blockStartOrd = docOrd;
    const uint32_t base = (docOrd == 0) ? 0 : (uint32_t) docBuf[Postings::DOCS_BLOCK_SIZE - 1];
    const uint32_t docBase = base + (blockStartOrd == 0 ? 0 : 1);

    seekToBlockBody();
    readyBlockBodyBytes = 0;
    bodyReady = true;
    const int8_t token = (int8_t) *docIS.ptr();
    if (token > 0) {
      return false;
    }

    const int32_t numWords = token == Postings::DOC_BLOCK_CONTIGUOUS ? 0 : -token;
    const char* wordsPtr = docIS.ptr() + 1;
    uint32_t blockLast;
    if (token == Postings::DOC_BLOCK_CONTIGUOUS) {
      blockLast = docBase + Postings::DOCS_BLOCK_SIZE - 1;
    } else {
      assert(numWords > 0);
      uint64_t lastWord = loadWord64(wordsPtr + (int64_t) (numWords - 1) * 8);
      assert(lastWord != 0);
      blockLast = docBase + (uint32_t) (numWords - 1) * 64
                  + (63 - (uint32_t) std::countl_zero(lastWord));
    }

    enterDocOnlyResidentBlock(blockStartOrd, docBase, blockLast, numWords);
    if (!advanceDocOnlyResident(target)) {
      return false;
    }
    return true;
  }

  bool orDocOnlyResidentIntoBitSet(std::span<uint64_t> bits, int32_t bitsBase,
                                   int32_t upTo) {
    assert(docBlockResident);
    const int32_t minDoc = docid < 0 ? (int32_t) residentDocBase : docid;
    if (upTo <= minDoc) {
      return false;
    }

    int32_t lastDoc = 0;
    int32_t ordAfter = 0;
    if (!findResidentLastBefore(upTo, minDoc, lastDoc, ordAfter)) {
      // No doc below upTo (a leading gap spans the window). Leave the cursor
      // alone: docid must only ever rest on a doc already reported to the
      // consumer, or nextDocOnly()'s docid+1 scan would skip a live doc.
      return false;
    }

    const int32_t clipDoc = std::max(minDoc, bitsBase);
    const int32_t emitTo = std::min(upTo, (int32_t) residentBlockLast + 1);
    if (clipDoc < emitTo) {
      if (residentNumWords == 0) {
        orBitRange(bits, clipDoc - bitsBase, emitTo - clipDoc);
      } else {
        orShiftedWordsRange(bits, bitsBase, residentWordsPtr, residentNumWords,
                            residentDocBase, clipDoc, emitTo);
      }
      skipCount(SkipStats::countBulkFillWordBlocks);
    }

    if (lastDoc == (int32_t) residentBlockLast) {
      finishDocOnlyResidentBlock();
      return true;
    }

    positionDocOnlyResidentAt(lastDoc, ordAfter, true);
    return false;
  }

  // Consume whole word-encoded full blocks lying entirely below upTo, OR-ing
  // their bits into `bits` without expanding to docBuf. A word/contiguous
  // block that straddles upTo enters resident state for clipped window fills.
  // Stops with the stream ready for the regular decode on a packed block, the
  // tail, or a word/contiguous block that begins at or above upTo. Returns
  // whether any block was consumed.
  bool orWholeWordBlocks(std::span<uint64_t> bits, int32_t bitsBase,
                         int32_t upTo) {
    bool consumed = false;
    while (docfreq - docOrd >= Postings::DOCS_BLOCK_SIZE) {
      seekToBlockBody();
      readyBlockBodyBytes = 0;
      bodyReady = true;  // the stream is committed past the headers either way
      const int8_t token = (int8_t) *docIS.ptr();
      if (token > 0) {
        break;  // packed: the regular decode path takes it from here
      }
      const uint32_t base = (docOrd == 0) ? 0 : (uint32_t) docBuf[Postings::DOCS_BLOCK_SIZE - 1];
      const uint32_t docBase = base + (docOrd == 0 ? 0 : 1);
      const int32_t numWords = token == Postings::DOC_BLOCK_CONTIGUOUS ? 0 : -token;
      uint32_t blockLast;
      if (token == Postings::DOC_BLOCK_CONTIGUOUS) {
        blockLast = docBase + Postings::DOCS_BLOCK_SIZE - 1;
      } else {
        uint64_t lastWord;
        memcpy(&lastWord, docIS.ptr() + 1 + (int64_t) (numWords - 1) * 8, 8);
        assert(lastWord != 0);
        blockLast = docBase + (uint32_t) (numWords - 1) * 64
                    + (63 - (uint32_t) std::countl_zero(lastWord));
      }
      if ((int32_t) docBase >= upTo) {
        break;
      }
      if ((int32_t) blockLast >= upTo) {
        enterDocOnlyResidentBlock(docOrd, docBase, blockLast, numWords);
        consumed = true;
        break;
      }

      const int32_t clipDoc = std::max((int32_t) docBase, bitsBase);
      if (token == Postings::DOC_BLOCK_CONTIGUOUS) {
        if ((int32_t) blockLast >= clipDoc) {
          orBitRange(bits, clipDoc - bitsBase, (int32_t) blockLast - clipDoc + 1);
        }
      } else {
        orShiftedWords(bits, bitsBase, docIS.ptr() + 1, numWords, docBase, clipDoc);
      }
      skipCount(SkipStats::countBulkFillWordBlocks);

      // Wholesale consume, mirroring skipToBlock's re-anchor shape: the next
      // decode's cross-block base is this block's last id in docBuf.
      docIS.skip(1 + (int64_t) numWords * 8);
      if (hasFreqs) {
        auto bytesSkipped = IndexCodec::tfreqCodec.skipBlock(docIS.ptr(), docIS.left());
        docIS.skip(bytesSkipped);
        skipCount(SkipStats::docsOnlyFreqBlocksSkipped);
        tfreqOrd += Postings::DOCS_BLOCK_SIZE;
      }
      bodyReady = false;
      readyBlockBodyBytes = 0;
      const int32_t blockStartOrd = docOrd;
      docOrd += Postings::DOCS_BLOCK_SIZE;
      tfreqBufIdx = tfreqBufEnd = 0;
      tfreq = 1;
      docid = (int32_t) blockLast;
      blockMode = true;
      docBuf[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) blockLast;
      docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
      nextL0Base = blockLast;
      nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
      if (isL1Boundary(nextL0Block)) {
        nextL1Group = nextL0Block / L1_PERIOD;
        nextL1Base = nextL0Base;
      }
      consumed = true;
    }
    return consumed;
  }

public:
  // Bulk count fill (the Lucene PostingsEnum#intoBitSet shape): consume every
  // remaining doc below upTo, setting bit (doc - bitsBase) for docs at or past
  // bitsBase; docs below bitsBase are consumed unrecorded (the caller's window
  // can open past the current position). Docs-only consumption: freq blocks
  // are skipped and this enum can never serve termFreq() or positions again.
  // Whole word-encoded blocks below upTo are OR'd straight from the stream;
  // everything else decodes and scatters per doc.
  void intoBitSet(std::span<uint64_t> bits, int32_t bitsBase, int32_t upTo) {
    assert(!positionTrackingEnabled);
    assert(!scoredProbeActive);
    docsOnlyConsumed = true;
    for (;;) {
      if (docBlockResident) {
        if (orDocOnlyResidentIntoBitSet(bits, bitsBase, upTo)) {
          continue;
        }
        return;
      }
      if ((docid < 0 || blockMode) && docBufIdx >= docBufEnd
          && orWholeWordBlocks(bits, bitsBase, upTo)) {
        continue;
      }
      auto blockDocs = peekDocOnlyBlock();
      int32_t available = (int32_t) blockDocs.size();
      if (available == 0) {
        return;
      }

      int32_t used = 0;
      while (used < available && blockDocs[(size_t) used] < bitsBase) {
        used++;
      }
      int32_t firstEmit = used;
      while (used < available && blockDocs[(size_t) used] < upTo) {
        used++;
      }

      int32_t emit = used - firstEmit;
      if (emit > 0) {
        skipCount(SkipStats::countBulkFillBlocks);
        if (SkipStats::enabled) {
          SkipStats::countBulkFillDocs += emit;
        }
        orDocBits(bits, blockDocs.data() + firstEmit, emit, bitsBase);
      }

      if (used == 0) {
        return;
      }
      consumeDocOnlyBlock(used);

      if (used < available) {
        return;
      }
    }
  }

  // Return remaining decoded docs/freqs from the current block. This is a
  // peek-only block-mode API: consumeDocFreqBlock() is the only cursor mutation
  // past the returned span. It may call nextDoc() to decode the next block, and
  // therefore may position docId()/termFreq() at the first returned doc. It does
  // not keep position/cumulative-tf metadata synchronized because score-only
  // block consumers do not need it. Spans are valid until the next cursor call.
  // Impact threshold ownership remains with TermQuery::Scorer; this exposes raw
  // decoded postings and does not know minCompetitiveScore.
  std::pair<std::span<const int32_t>, std::span<const int32_t>> peekDocFreqBlock() {
    assert(!docBlockResident);
    assert(!docsOnlyConsumed);
    if (scoredProbeActive) {
      materializeScoredProbeBlock();
    }
    if (docid == PostingsReader::END) {
      return {};
    }

    int32_t start = 0;
    if (!blockMode) {
      if (docid < 0) {
        int32_t doc = nextDoc();
        if (doc == PostingsReader::END) {
          return {};
        }
      }
      start = docBufIdx - 1;
    } else {
      if (docBufIdx >= docBufEnd) {
        int32_t doc = nextDoc();
        if (doc == PostingsReader::END) {
          return {};
        }
        start = docBufIdx - 1;
      } else {
        start = docBufIdx;
      }
    }

    if (start >= docBufEnd) {
      return {};
    }

    if (!hasFreqs) {
      std::fill(tfreqBuf + start, tfreqBuf + docBufEnd, 1);
    }

    int32_t emitted = docBufEnd - start;
    return {
      std::span<const int32_t>(docBuf + start, (size_t) emitted),
      std::span<const int32_t>(tfreqBuf + start, (size_t) emitted)
    };
  }

  std::span<const int32_t> peekDocBlock() {
    return peekDocOnlyBlock();
  }

  void consumeDocFreqBlock(int32_t n) {
    assert(n >= 0);
    assert(!scoredProbeActive);
    if (n == 0) {
      return;
    }
    assert(!docBlockResident);
    assert(!docsOnlyConsumed);
    assert(docid != PostingsReader::END);

    int32_t start = 0;
    bool countedCurrent = false;
    if (!blockMode) {
      assert(docid >= 0);
      start = docBufIdx - 1;
      countedCurrent = true;
    } else {
      start = docBufIdx;
    }

    int32_t limit = start + n;
    assert(start >= 0);
    assert(limit <= docBufEnd);

    int32_t newlyCounted = n - (countedCurrent ? 1 : 0);
    assert(newlyCounted >= 0);
    docOrd += newlyCounted;
    if (hasFreqs) {
      tfreqOrd += newlyCounted;
    }
    docBufIdx = limit;
    if (hasFreqs) {
      tfreqBufIdx = limit;
      tfreq = tfreqBuf[limit - 1];
    } else {
      tfreq = 1;
    }
    docid = docBuf[limit - 1];
    blockMode = true;
    if (positionTrackingEnabled) {
      // Preserve this API's contract: block consumers do not repair position
      // metadata. Since the current doc changed, any deferred repair for the
      // previous cursor position is no longer meaningful.
      clearPendingPositionRepair();
    }
  }

  void finishScoredProbeAtBlockEnd() {
    assert(scoredProbeActive);
    docid = (int32_t) scoredProbeBlockLast;
    docOrd = scoredProbeBlockStartOrd + Postings::DOCS_BLOCK_SIZE;
    docBuf[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) scoredProbeBlockLast;
    docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
    tfreqOrd = docOrd;
    tfreqBufIdx = tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
    blockMode = true;
    clearScoredProbe();
  }

  template <bool TrimProbeWrapper>
  SOLUX_INLINE int32_t positionScoredProbeAtGEQ(int32_t target) {
    assert(scoredProbeActive);
    assert(target > docid);
    assert(target <= (int32_t) scoredProbeBlockLast);
    int32_t landing;
    int32_t idx;
    const int32_t numWords = scoredProbeNumWords();
    if (numWords < 0) {
      const int32_t start = docOrd - scoredProbeBlockStartOrd;
      idx = DecodedSuccessor::index(
          docBuf, start, Postings::DOCS_BLOCK_SIZE, target);
      assert(idx < Postings::DOCS_BLOCK_SIZE);
      landing = docBuf[idx];
    } else if (numWords == 0) {
      landing = std::max(target, (int32_t) scoredProbeDocBase);
      idx = landing - (int32_t) scoredProbeDocBase;
    } else {
      int32_t bitIndex = target - (int32_t) scoredProbeDocBase;
      if (bitIndex < 0) {
        bitIndex = 0;
      }
      const int32_t targetWord = bitIndex >> 6;
      const int32_t targetBit = bitIndex & 63;
      int32_t rankWord = tfreqBufIdx;
      int32_t ordBeforeWord = tfreqOrd;
      assert(rankWord <= targetWord);
      while (rankWord < targetWord) {
        uint64_t word = loadWord64(
            scoredProbeWordsPtr + (int64_t) rankWord * 8);
        ordBeforeWord += (int32_t) std::popcount(word);
        rankWord++;
      }
      for (;;) {
        assert(rankWord < numWords);
        uint64_t word = loadWord64(
            scoredProbeWordsPtr + (int64_t) rankWord * 8);
        uint64_t hits = word;
        if (rankWord == targetWord) {
          hits &= ~lowBitsMask(targetBit);
        }
        if (hits != 0) {
          const int32_t bit = (int32_t) std::countr_zero(hits);
          landing = (int32_t) scoredProbeDocBase
                    + (rankWord << 6) + bit;
          idx = ordBeforeWord
                - scoredProbeBlockStartOrd
                + (int32_t) std::popcount(word & lowBitsMask(bit));
          break;
        }
        ordBeforeWord += (int32_t) std::popcount(word);
        rankWord++;
      }
      tfreqBufIdx = rankWord;
      tfreqOrd = ordBeforeWord;
    }
    assert(idx >= 0 && idx < Postings::DOCS_BLOCK_SIZE);
    assert(landing >= target && landing <= (int32_t) scoredProbeBlockLast);
    docid = landing;
    docOrd = scoredProbeBlockStartOrd + idx + 1;
    docBufIdx = idx + 1;
    if constexpr (!TrimProbeWrapper) {
      // The block entry publishes this once. Retain the old per-landing store
      // only for the same-binary attribution path.
      docBufEnd = Postings::DOCS_BLOCK_SIZE;
    }
    if (scoredProbeFreqsDecoded()) {
      tfreq = hasFreqs ? tfreqBuf[idx] : 1;
      tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
    }
    blockMode = false;
    return docid;
  }

  template <bool TrimProbeWrapper>
  void beginScoredProbeBlock(int32_t target) {
    assert(!scoredProbeActive);
    assert(docfreq - docOrd >= Postings::DOCS_BLOCK_SIZE);
    const int32_t blockStartOrd = docOrd;
    const uint32_t base = docOrd == 0
        ? 0 : (uint32_t) docBuf[Postings::DOCS_BLOCK_SIZE - 1];
    const uint32_t docBase = base + (blockStartOrd == 0 ? 0 : 1);

    uint32_t bodyBytes = seekToScoredProbeBlockBody();
    const char* bodyStart = docIS.ptr();
    const int8_t token = (int8_t) *docIS.ptr();
    docIS.skip(1);
    int32_t numWords;
    const char* wordsPtr = nullptr;
    uint32_t blockLast;
    if (token > 0) {
      uint32_t outSz = Postings::DOCS_BLOCK_SIZE;
      auto bytesRead = IndexCodec::docCodec.decodeBlock(
          docIS.ptr(), docIS.left(), (uint32_t*) docBuf, outSz, base);
      docIS.skip(bytesRead);
      assert(outSz == Postings::DOCS_BLOCK_SIZE);
      blockLast = (uint32_t) docBuf[Postings::DOCS_BLOCK_SIZE - 1];
      numWords = -1;
      skipCount(SkipStats::docBlocksDecoded);
    } else if (token == Postings::DOC_BLOCK_CONTIGUOUS) {
      blockLast = docBase + Postings::DOCS_BLOCK_SIZE - 1;
      numWords = 0;
      skipCount(SkipStats::scoredWordProbeAdvances);
    } else {
      numWords = -token;
      assert(numWords > 0);
      wordsPtr = docIS.ptr();
      uint64_t lastWord = loadWord64(
          wordsPtr + (int64_t) (numWords - 1) * 8);
      assert(lastWord != 0);
      blockLast = docBase + (uint32_t) (numWords - 1) * 64
                  + (63 - (uint32_t) std::countl_zero(lastWord));
      docIS.skip((int64_t) numWords * 8);
      skipCount(SkipStats::scoredWordProbeAdvances);
    }

    scoredProbeActive = true;
    scoredProbeWordsPtr = wordsPtr;
    scoredProbeDocBase = docBase;
    scoredProbeBlockLast = blockLast;
    scoredProbeBlockStartOrd = blockStartOrd;
    assert(numWords + 1 >= 0
           && (uint32_t) (numWords + 1) <= SCORED_PROBE_REP_MASK);
    scoredProbeState = (uint32_t) (numWords + 1);

    docBuf[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) blockLast;
    docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
    tfreqOrd = blockStartOrd;
    tfreqBufIdx = 0;
    tfreqBufEnd = 0;
    positionScoredProbeAtGEQ<TrimProbeWrapper>(target);

    if (hasFreqs) {
      uint32_t docBytes = (uint32_t) (docIS.ptr() - bodyStart);
      assert(docBytes < bodyBytes);
      uint32_t freqBytes = bodyBytes - docBytes;
      tfreqBufEnd = (int32_t) freqBytes;
      docIS.skip(freqBytes);
    } else {
      assert((uint32_t) (docIS.ptr() - bodyStart) == bodyBytes);
    }
    nextL0Base = blockLast;
    nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
    if (isL1Boundary(nextL0Block)) {
      nextL1Group = nextL0Block / L1_PERIOD;
      nextL1Base = nextL0Base;
    }
    skipCount(SkipStats::scoredProbeAdvances);
  }

  // Probe-style scored advance for sorted membership tests. Full blocks remain
  // resident in their native doc representation, while the tfreq block is
  // skipped by length and decoded only if termFreq() is requested. A probe
  // landing is not itself a survivor: callers may have more conjunction
  // clauses to test. The resident state deliberately survives caller
  // batch/window boundaries.
  template <bool TrimProbeWrapper>
  SOLUX_INLINE int32_t advanceScoredProbeImpl(int32_t target) {
    assert(target > docid);
    assert(!docsOnlyConsumed);
    assert(!docBlockResident);
    assert(!positionTrackingEnabled);
    if constexpr (!TrimProbeWrapper) {
      skipCount(SkipStats::advanceCalls);
    }

    if (scoredProbeActive) {
      if (target <= (int32_t) scoredProbeBlockLast) {
        return positionScoredProbeAtGEQ<TrimProbeWrapper>(target);
      }
      finishScoredProbeAtBlockEnd();
    }
    if (docBufEnd > 0 && target <= docBuf[docBufEnd - 1]) {
      return advanceScoredNoPositionsFromReadyBlock(target);
    }
    if constexpr (TrimProbeWrapper) {
      // Failure of the decoded-remainder check above already established that
      // target is beyond docBufEnd.
      if (nextL0Block < numDocBlocks) {
        skipToBlock(target);
      }
    } else {
      if (nextL0Block < numDocBlocks
          && (docBufEnd == 0 || target > docBuf[docBufEnd - 1])) {
        skipToBlock(target);
      }
    }
    if (docOrd >= docfreq || docfreq - docOrd < Postings::DOCS_BLOCK_SIZE) {
      return advanceScoredNoPositionsFromReadyBlock(target);
    }
    beginScoredProbeBlock<TrimProbeWrapper>(target);
    assert(target <= (int32_t) scoredProbeBlockLast);
    return docid;
  }

  int32_t advanceScoredProbe(int32_t target) SOLUX_INLINE {
#if SOLUX_PROBE_WRAPPER_TRIMS && SOLUX_PROBE_CONSTANT_HOOKS
    if (disableProbeWrapperTrimsForTests) {
      return advanceScoredProbeImpl<false>(target);
    }
#endif
#if SOLUX_PROBE_WRAPPER_TRIMS
    return advanceScoredProbeImpl<true>(target);
#else
    return advanceScoredProbeImpl<false>(target);
#endif
  }

  // Fused membership/freq fetch for the conjunction clause pass. Once a
  // packed probe has promoted both planes to ordinary decoded buffers, keep
  // later candidates on the short decoded-remainder path and return the
  // current freq directly. Native word/contiguous probes retain the lazy
  // ownership seam in termFreq().
  bool matchScoredProbe(int32_t target, int32_t& freq) SOLUX_INLINE {
    assert(target >= 0);
    assert(!docsOnlyConsumed);
    assert(!docBlockResident);
    assert(!positionTrackingEnabled);

    if (target < docid) {
      return false;
    }
    if (target == docid) {
      freq = scoredProbeActive ? termFreq() : tfreq;
      return true;
    }
    if (!scoredProbeActive && docBufEnd > 0
        && target <= docBuf[docBufEnd - 1]) {
      skipCount(SkipStats::advanceCalls);
      if (advanceScoredNoPositionsFromReadyBlock(target) != target) {
        return false;
      }
      freq = tfreq;
      return true;
    }
    if (advanceScoredProbe(target) != target) {
      return false;
    }
    freq = termFreq();
    return true;
  }


  // Reset the doc decoder to the block that may contain target. Position state
  // is repaired by cumulativeTermFreq; the position stream itself stays lazy.
  // Untracked enums skip position metadata wholesale at both header levels.
  template <bool TrackPositions>
  void skipToBlock(int32_t target) {
    assert(!docBlockResident);
    assert(!scoredProbeActive);
    if constexpr (TrackPositions) {
      assert(positionTrackingEnabled || disableSlimL0WalkForTests);
    } else {
      assert(!positionTrackingEnabled);
    }
    blockMode = false;
    if constexpr (TrackPositions) {
      clearPendingPositionRepair();
      pendingPosSeekValid = false;
    }
    const char* const streamStart = docIS.ptr(0);
    const char* const end = docIS.ptr(endOfDocs);
    const char* p = docIS.ptr();
    int32_t block = nextL0Block;
    uint32_t prevLastDoc = nextL0Base;
    int64_t cumTf = 0;
    if constexpr (TrackPositions) {
      cumTf = nextL0CumTf;
    }

    auto walkL0To = [&](int32_t maxBlock) -> bool {
      while (block < maxBlock && block < numDocBlocks) {
        skipCount(SkipStats::l0HeaderSteps);
        uint32_t headerLen = InputStream::readVint(p, end);
        const char* headerEnd = p + headerLen;
        assert(headerEnd <= end);
        uint32_t lastDocDelta = readVint15(p, headerEnd);
        uint32_t blockLastDoc = prevLastDoc + lastDocDelta;
        uint64_t blockByteLen = readVlong15(p, headerEnd);
        int64_t blockTfSum = 0;
        uint64_t blockPosByteOff = 0;
        if constexpr (TrackPositions) {
          int32_t blockDocCount = std::min(
              Postings::DOCS_BLOCK_SIZE,
              docfreq - block * Postings::DOCS_BLOCK_SIZE);
          blockTfSum = blockDocCount;
          if (hasPositions) {
            blockTfSum += InputStream::readVint(p, headerEnd);
            blockPosByteOff = InputStream::readVlong(p, headerEnd);
          }
        }
        p = headerEnd;

        const char* body = headerEnd;
        if (target <= (int32_t) blockLastDoc) {
          docIS.seek(body - streamStart);
          docOrd = block * Postings::DOCS_BLOCK_SIZE;
          tfreqOrd = docOrd;
          if constexpr (TrackPositions) {
            cumulativeTermFreq = cumTf;
            if (positionTrackingEnabled) {
              // Publish a forward seek anchor. PosEnum applies it lazily before
              // serving positions, preserving the direct skip optimization
              // without making the doc cursor mutate position decoder state.
              pendingPosBlockOrd =
                  cumTf - (cumTf % Postings::POSITIONS_BLOCK_SIZE);
              pendingPosAbsoluteOffset =
                  posStartLoc + (int64_t) blockPosByteOff;
              pendingPosSeekValid = true;
            }
          }
          docBuf[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) prevLastDoc;
          // Force a decode on the next nextDoc().
          docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
          tfreqBufIdx = tfreqBufEnd = 0;
          assert(blockByteLen
                 <= (uint64_t) std::numeric_limits<uint32_t>::max());
          readyBlockBodyBytes = (uint32_t) blockByteLen;
          nextL0Block = block;
          nextL0Base = prevLastDoc;
          if constexpr (TrackPositions) {
            nextL0CumTf = cumTf;
          }
          if (isL1Boundary(block)) {
            nextL1Group = block / L1_PERIOD;
            nextL1Base = prevLastDoc;
          }
          bodyReady = true;
          return true;
        }

        p = body + (int64_t) blockByteLen;
        assert(p <= end);
        prevLastDoc = blockLastDoc;
        if constexpr (TrackPositions) {
          cumTf += blockTfSum;
        }
        block++;
      }
      return false;
    };

    if (!isL1Boundary(block)) {
      if constexpr (!TrackPositions) {
        int32_t groupStartBlock = (block / L1_PERIOD) * L1_PERIOD;
        if ((int64_t) target - (int64_t) prevLastDoc
                >= L0_CHECKPOINT_MIN_SKIP_DOCS
            && l0CheckpointGroupStartBlock == groupStartBlock) {
          L0Checkpoint checkpoint;
          if (consultL0Checkpoints(
                  l0CheckpointEntries, l0CheckpointCount, (uint32_t) target,
                  block - groupStartBlock, checkpoint)) {
            p = l0CheckpointGroupBody + checkpoint.bodyOffset;
            block = groupStartBlock + checkpoint.blockIdxInGroup;
            prevLastDoc = checkpoint.key;
            skipCount(SkipStats::l0CheckpointJumps);
          }
        }
      }
      int32_t nextGroupBlock = ((block / L1_PERIOD) + 1) * L1_PERIOD;
      if (walkL0To(nextGroupBlock)) {
        return;
      }
    }

    while (block < numDocBlocks) {
      assert(isL1Boundary(block));
      skipCount(SkipStats::l1GroupSteps);
      int32_t group = block / L1_PERIOD;
      nextL1Group = group;
      nextL1Base = prevLastDoc;

      uint32_t groupHeaderLen = InputStream::readVint(p, end);
      const char* groupHeaderEnd = p + groupHeaderLen;
      assert(groupHeaderEnd <= end);
      uint32_t groupLastDocDelta = readVint15(p, groupHeaderEnd);
      uint32_t groupLastDoc = prevLastDoc + groupLastDocDelta;
      uint64_t groupByteLen = readVlong15(p, groupHeaderEnd);
      assert(p < groupHeaderEnd);
      p++;  // per-group packed-block count: no cursor consumer
      assert(p < groupHeaderEnd);
      uint8_t entryCount = (uint8_t) *p++;
      const char* checkpointEntries = p;
      assert(p + (size_t) entryCount * sizeof(uint64_t) <= groupHeaderEnd);
      p += (size_t) entryCount * sizeof(uint64_t);
      int32_t groupBlockCount = std::min(L1_PERIOD, numDocBlocks - block);
      int32_t groupStartBlock = block;
      l0CheckpointEntries = checkpointEntries;
      l0CheckpointCount = entryCount;
      l0CheckpointGroupBody = groupHeaderEnd;
      l0CheckpointGroupStartBlock = groupStartBlock;
      int64_t groupTfSum = 0;
      if constexpr (TrackPositions) {
        int32_t groupDocCount =
            std::min(L1_DOCS, docfreq - group * L1_DOCS);
        groupTfSum = groupDocCount;
        if (hasPositions) {
          groupTfSum += InputStream::readVint(p, groupHeaderEnd);
        }
        if (hasFreqs && hasNorms) {
          uint32_t code = InputStream::readVint(p, groupHeaderEnd);
          uint32_t frontierCount = code >> 1;
          uint32_t tfWidth = (code & 1u) != 0 ? 4u : 2u;
          assert(frontierCount > 0 && frontierCount <= 256);
          size_t payloadBytes =
              frontierCount + (size_t) frontierCount * tfWidth;
          assert(p + payloadBytes <= groupHeaderEnd);
          p += payloadBytes;
        } else if (hasFreqs) {
          auto spanImpact = InputStream::readVint(p, groupHeaderEnd);
          unused(spanImpact);
        } else if (hasNorms) {
          auto spanMinNorm = InputStream::readVint(p, groupHeaderEnd);
          unused(spanMinNorm);
        }
        assert(p == groupHeaderEnd);
      } else {
        p = groupHeaderEnd;
      }

      const char* groupBody = groupHeaderEnd;
      if (target <= (int32_t) groupLastDoc) {
        p = groupBody;
        if constexpr (!TrackPositions) {
          L0Checkpoint checkpoint;
          if (consultL0Checkpoints(
                  checkpointEntries, entryCount, (uint32_t) target, -1,
                  checkpoint)) {
            p = groupBody + checkpoint.bodyOffset;
            assert(p < groupBody + (int64_t) groupByteLen);
            block = groupStartBlock + checkpoint.blockIdxInGroup;
            prevLastDoc = checkpoint.key;
            skipCount(SkipStats::l0CheckpointJumps);
          }
        }
        if (walkL0To(groupStartBlock + groupBlockCount)) {
          return;
        }
        assert(false);
        return;
      }

      p = groupBody + (int64_t) groupByteLen;
      assert(p <= end);
      prevLastDoc = groupLastDoc;
      if constexpr (TrackPositions) {
        cumTf += groupTfSum;
      }
      block += groupBlockCount;
      nextL1Group = block / L1_PERIOD;
      nextL1Base = prevLastDoc;
    }

    docIS.seek(p - streamStart);
    docOrd = docfreq;
    tfreqOrd = docfreq;
    if constexpr (TrackPositions) {
      cumulativeTermFreq = cumTf;
    }
    docBufIdx = docBufEnd = 0;
    tfreqBufIdx = tfreqBufEnd = 0;
    nextL0Block = numDocBlocks;
    nextL0Base = prevLastDoc;
    if constexpr (TrackPositions) {
      nextL0CumTf = cumTf;
    }
    nextL1Group = docGroupCount();
    nextL1Base = prevLastDoc;
    readyBlockBodyBytes = 0;
    bodyReady = false;
  }

  void skipToBlock(int32_t target) {
    if (!positionTrackingEnabled && !disableSlimL0WalkForTests) {
      skipToBlock<false>(target);
    } else {
      skipToBlock<true>(target);
    }
  }

private:
  template <bool DOCS_ONLY>
  int32_t advanceImpl(int32_t target) {
    assert(docid < target);
    if constexpr (DOCS_ONLY) {
      assert(!scoredProbeActive);
    } else if (scoredProbeActive) {
      materializeScoredProbeBlock();
    }
    skipCount(SkipStats::advanceCalls);
    if constexpr (DOCS_ONLY) {
      assert(!positionTrackingEnabled);
      docsOnlyConsumed = true;
      if (docBlockResident) {
        if (advanceDocOnlyResident(target)) {
          return docid;
        }
      }
    } else {
      assert(!docBlockResident);
      assert(!docsOnlyConsumed);
    }
    if (nextL0Block < numDocBlocks
        && (docBufEnd == 0 || target > docBuf[docBufEnd - 1])) {
      skipToBlock(target);
    }
    if constexpr (DOCS_ONLY) {
      if (docBufIdx >= docBufEnd && tryEnterDocOnlyResidentBlock(target)) {
        return docid;
      }
    }
    if (docBufIdx >= docBufEnd) {
      // fresh block (or END) after the block-level skip
      if (nextDocImpl<DOCS_ONLY>() >= target) {
        return docid;
      }
    }
    if (docBuf[docBufEnd - 1] < target) {
      // target is past the final decoded block (last block of the list, or a
      // pulsed posting): walk out to END the simple way.
      while (docid < target) {
        nextDocImpl<DOCS_ONLY>();
      }
      return docid;
    }

    // The target lies in the decoded remainder: jump to it directly instead of
    // a per-doc nextDoc() walk. Plain term scorers do not consume positions, so
    // they skip cumulative-tf repair entirely.
    blockMode = false;
    const int32_t start = docBufIdx;
    const int32_t j =
        DecodedSuccessor::index(docBuf, start, docBufEnd, target);
    assert(j < docBufEnd);
    const int32_t consumed = j + 1 - start;
    docOrd += consumed;
    if constexpr (DOCS_ONLY) {
      if (hasFreqs) {
        tfreqOrd += consumed;
        tfreqBufIdx = j + 1;
      }
      tfreq = 1;
    } else {
      if (hasFreqs) {
        tfreq = tfreqBuf[j];
        tfreqOrd += consumed;
        tfreqBufIdx = j + 1;
        if (positionTrackingEnabled) {
          markPendingPositionRepair(start, j + 1);
        }
      } else {
        tfreq = 1;
        if (positionTrackingEnabled) {
          markPendingPositionRepair(start, j + 1);
        }
      }
    }
    docBufIdx = j + 1;
    docid = docBuf[j];
    return docid;
  }

public:
  // Strict (like the Scorer / Lucene PostingsEnum contract): target must be beyond
  // the current doc.  Callers: ConjunctionScorer / MandOpt / MandNot, PhraseQuery,
  // ConstantScoreQuery, the column-join iterators.
  int32_t advance(int32_t target) {
    return advanceImpl<false>(target);
  }

  // Docs-only strict advance: same cursor contract as advance(), but skips freqs
  // and permanently switches the enum to docs-only consumption.
  int32_t advanceDocOnly(int32_t target) {
    return advanceImpl<true>(target);
  }

  // The current decoded block begins at the already-positioned doc.  The
  // caller may inspect remaps before choosing beginPositionDeltaBatch() or the
  // ordinary per-doc position path.  The spans remain valid until the next
  // doc-block decode.
  DocFreqBlock currentDocFreqBlock() {
    assert(!docBlockResident);
    assert(!docsOnlyConsumed);
    assert(!scoredProbeActive);
    assert(!positionBatchActive);
    assert(hasPositions);
    assert(hasFreqs);
    assert(positionTrackingEnabled);
    assert(!blockMode);
    assert(docid >= 0 && docid != PostingsReader::END);
    int32_t start = docBufIdx - 1;
    assert(start >= 0 && start < docBufEnd);
    int32_t count = docBufEnd - start;
    return {
      std::span<const int32_t>(docBuf + start, (size_t) count),
      std::span<const int32_t>(tfreqBuf + start, (size_t) count)
    };
  }

private:
  // Move the doc/freq cursor across a position batch after PosEnum has aligned
  // the position stream at the first current doc.
  int64_t beginPositionDeltaBatchDocs(int32_t docCount) {
    assert(docCount > 0);
    assert(!positionBatchActive);
    assert(hasPositions && hasFreqs && positionTrackingEnabled);
    assert(!blockMode);
    int32_t start = docBufIdx - 1;
    int32_t limit = start + docCount;
    assert(start >= 0 && limit <= docBufEnd);

    int64_t tfSum = 0;
    for (int32_t i = start; i < limit; i++) {
      tfSum += (uint32_t) tfreqBuf[i];
    }
    assert(tfSum >= docCount);

    int32_t newlyCounted = docCount - 1;
    docOrd += newlyCounted;
    tfreqOrd += newlyCounted;
    docBufIdx = limit;
    tfreqBufIdx = limit;
    docid = docBuf[limit - 1];
    tfreq = tfreqBuf[limit - 1];
    blockMode = true;

    cumulativeTermFreq += tfSum - (uint32_t) tfreqBuf[start];
    posOrdStart = cumulativeTermFreq - tfreq;
    clearPendingPositionRepair();
    if (docsSize != 0 && limit == docBufEnd) {
      assert(cumulativeTermFreq == nextL0CumTf);
    }
    positionBatchActive = true;
    return cumulativeTermFreq;
  }
};

// The DOCS tier has no frequency or position cursor state. It owns only the
// decoded-doc buffer, doc cursor, resident word-block state, and L0 navigation
// state needed by nextDoc(), advance(), and bulk bitset production.
template<>
class BasicDocsEnum<DocsEnumTier::DOCS> final : public DocsEnumMeta {
  const char* residentWordsPtr = nullptr;
  uint32_t residentDocBase = 0;
  uint32_t residentBlockLast = 0;
  int32_t residentBlockStartOrd = 0;
  int32_t residentNumWords = 0;

  int32_t docOrd = 0;
  int32_t docBufIdx = 0;
  int32_t docBufEnd = 0;
  int32_t docid = -1;
  int32_t nextL0Block = 0;
  uint32_t nextL0Base = 0;
  bool blockMode = false;
  bool docBlockResident = false;
  bool bodyReady = false;
  const char* l0CheckpointEntries = nullptr;
  const char* l0CheckpointGroupBody = nullptr;
  int32_t l0CheckpointGroupStartBlock = -1;
  uint8_t l0CheckpointCount = 0;

  // +8: expandDocWords writes branchless 8-wide rows; if the last byte of the
  // last word has popcount 0 (common - it just means the block's final doc
  // isn't in that byte's bit range), the unconditional dst[0..7] write for
  // that byte lands entirely past the true end, needing indices up to
  // DOCS_BLOCK_SIZE + 7.
  int32_t db[Postings::DOCS_BLOCK_SIZE + 8];

  static bool isL1Boundary(int32_t block) {
    return (block % L1_PERIOD) == 0;
  }

  static uint64_t lowBitsMask(int32_t bits) {
    assert(bits >= 0 && bits <= 64);
    if (bits == 0) {
      return 0;
    }
    if (bits == 64) {
      return ~0ULL;
    }
    return (1ULL << bits) - 1ULL;
  }

  static uint64_t loadWord64(const char* p) {
    uint64_t word;
    memcpy(&word, p, 8);
    return word;
  }

  void seekToBlockBody() {
    if (!bodyReady) {
      if (isL1Boundary(nextL0Block)) {
        auto groupHeaderLen = docIS.readVint();
        docIS.skip(groupHeaderLen);
      }
      auto headerLen = docIS.readVint();
      docIS.skip(headerLen);
    } else {
      bodyReady = false;
    }
  }

  static constexpr auto BITPOS = [] {
    std::array<std::array<uint8_t, 8>, 256> t{};
    for (int b = 0; b < 256; b++) {
      int n = 0;
      for (int i = 0; i < 8; i++) {
        if (b & (1 << i)) {
          t[(size_t) b][(size_t) n++] = (uint8_t) i;
        }
      }
    }
    return t;
  }();

  void expandDocWords(const char* p, int32_t numWords, uint32_t docBase) {
    int32_t* dst = db;
    for (int32_t w = 0; w < numWords; w++) {
      uint64_t word;
      memcpy(&word, p + (int64_t) w * 8, 8);
      const uint32_t wordBase = docBase + (uint32_t) (w << 6);
      for (int32_t b = 0; b < 8; b++) {
        const uint8_t byte = (uint8_t) (word >> (b * 8));
        const uint32_t byteBase = wordBase + (uint32_t) (b << 3);
        const uint8_t* row = BITPOS[byte].data();
        for (int32_t i = 0; i < 8; i++) {
          dst[i] = (int32_t) (byteBase + row[i]);
        }
        dst += std::popcount(byte);
      }
    }
    assert(dst - db == Postings::DOCS_BLOCK_SIZE);
  }

  void clearDocBlockResident() {
    docBlockResident = false;
    residentWordsPtr = nullptr;
    residentDocBase = 0;
    residentBlockLast = 0;
    residentBlockStartOrd = 0;
    residentNumWords = 0;
  }

  uint64_t residentWord(int32_t wordIndex) const {
    assert(docBlockResident);
    assert(residentNumWords > 0);
    assert(wordIndex >= 0 && wordIndex < residentNumWords);
    return loadWord64(residentWordsPtr + (int64_t) wordIndex * 8);
  }

  int32_t residentBlockEndOrd() const {
    assert(docBlockResident);
    return residentBlockStartOrd + Postings::DOCS_BLOCK_SIZE;
  }

  int32_t residentOrdinalAfterDoc(int32_t doc) const {
    assert(docBlockResident);
    assert(doc >= (int32_t) residentDocBase && doc <= (int32_t) residentBlockLast);
    if (residentNumWords == 0) {
      return residentBlockStartOrd + doc - (int32_t) residentDocBase + 1;
    }

    const int32_t bitIndex = doc - (int32_t) residentDocBase;
    const int32_t wordIndex = bitIndex >> 6;
    const int32_t bit = bitIndex & 63;
    int32_t ordInBlock = 0;
    for (int32_t w = 0; w < wordIndex; w++) {
      ordInBlock += (int32_t) std::popcount(residentWord(w));
    }
    const uint64_t word = residentWord(wordIndex);
    assert((word & (1ULL << bit)) != 0);
    ordInBlock += (int32_t) std::popcount(word & lowBitsMask(bit)) + 1;
    return residentBlockStartOrd + ordInBlock;
  }

  bool findResidentGEQ(int32_t target, int32_t& landing, int32_t& ordAfter) const {
    assert(docBlockResident);
    if (target > (int32_t) residentBlockLast) {
      return false;
    }
    if (residentNumWords == 0) {
      landing = std::max(target, (int32_t) residentDocBase);
      if (landing > (int32_t) residentBlockLast) {
        return false;
      }
      ordAfter = residentBlockStartOrd + landing - (int32_t) residentDocBase + 1;
      return true;
    }

    int32_t bitIndex = target - (int32_t) residentDocBase;
    if (bitIndex < 0) {
      bitIndex = 0;
    }
    int32_t startWord = bitIndex >> 6;
    int32_t startBit = bitIndex & 63;
    if (startWord >= residentNumWords) {
      return false;
    }

    int32_t prefix = 0;
    for (int32_t w = 0; w < residentNumWords; w++) {
      const uint64_t word = residentWord(w);
      if (w < startWord) {
        prefix += (int32_t) std::popcount(word);
        continue;
      }
      uint64_t probe = word;
      if (w == startWord) {
        probe &= ~lowBitsMask(startBit);
      }
      if (probe != 0) {
        const int32_t bit = (int32_t) std::countr_zero(probe);
        landing = (int32_t) residentDocBase + (w << 6) + bit;
        ordAfter = residentBlockStartOrd + prefix
                   + (int32_t) std::popcount(word & lowBitsMask(bit)) + 1;
        return true;
      }
      prefix += (int32_t) std::popcount(word);
    }
    return false;
  }

  bool findResidentLastBefore(int32_t upTo, int32_t minDoc,
                              int32_t& lastDoc, int32_t& ordAfter) const {
    assert(docBlockResident);
    if (upTo <= minDoc || minDoc > (int32_t) residentBlockLast) {
      return false;
    }
    if (residentNumWords == 0) {
      lastDoc = std::min((int32_t) residentBlockLast, upTo - 1);
      if (lastDoc < std::max(minDoc, (int32_t) residentDocBase)) {
        return false;
      }
      ordAfter = residentBlockStartOrd + lastDoc - (int32_t) residentDocBase + 1;
      return true;
    }

    int32_t startBit = std::max(0, minDoc - (int32_t) residentDocBase);
    int32_t endBit = upTo - (int32_t) residentDocBase;
    if (endBit <= startBit) {
      return false;
    }
    const int32_t maxBits = residentNumWords << 6;
    endBit = std::min(endBit, maxBits);

    int32_t found = -1;
    const int32_t firstWord = startBit >> 6;
    const int32_t lastWord = (endBit - 1) >> 6;
    for (int32_t w = firstWord; w <= lastWord && w < residentNumWords; w++) {
      uint64_t word = residentWord(w);
      const int32_t lo = w == firstWord ? (startBit & 63) : 0;
      const int32_t hi = w == lastWord ? ((endBit - 1) & 63) + 1 : 64;
      word &= lowBitsMask(hi) & ~lowBitsMask(lo);
      if (word != 0) {
        found = (w << 6) + (63 - (int32_t) std::countl_zero(word));
      }
    }
    if (found < 0) {
      return false;
    }
    lastDoc = (int32_t) residentDocBase + found;
    ordAfter = residentOrdinalAfterDoc(lastDoc);
    return true;
  }

  void positionResidentAt(int32_t landing, int32_t ordAfter, bool asBlockMode) {
    assert(docBlockResident);
    assert(ordAfter >= residentBlockStartOrd + 1);
    assert(ordAfter <= residentBlockEndOrd());
    docid = landing;
    docOrd = ordAfter;
    docBufIdx = ordAfter - residentBlockStartOrd;
    docBufEnd = Postings::DOCS_BLOCK_SIZE;
    blockMode = asBlockMode;
  }

  void finishResidentBlock() {
    assert(docBlockResident);
    docid = (int32_t) residentBlockLast;
    docOrd = residentBlockEndOrd();
    docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
    blockMode = true;
    db[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) residentBlockLast;
    clearDocBlockResident();
  }

  void materializeResidentBlock() {
    if (!docBlockResident) {
      return;
    }
    const int32_t idx = docOrd - residentBlockStartOrd;
    const uint32_t docBase = residentDocBase;
    const uint32_t blockLast = residentBlockLast;
    const int32_t numWords = residentNumWords;
    const char* wordsPtr = residentWordsPtr;
    if (numWords == 0) {
      for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
        db[i] = (int32_t) docBase + i;
      }
    } else {
      expandDocWords(wordsPtr, numWords, docBase);
    }
    assert(db[Postings::DOCS_BLOCK_SIZE - 1] == (int32_t) blockLast);
    clearDocBlockResident();
    docBufIdx = idx;
    docBufEnd = Postings::DOCS_BLOCK_SIZE;
  }

  void enterResidentBlock(int32_t blockStartOrd, uint32_t docBase,
                          uint32_t blockLast, int32_t numWords) {
    assert(!docBlockResident);
    assert(bodyReady);
    assert(docIS.ptr() < docIS.ptr(endOfDocs));

    bodyReady = false;
    docIS.skip(1);
    const char* wordsPtr = numWords == 0 ? nullptr : docIS.ptr();
    docIS.skip((int64_t) numWords * 8);
    if (hasFreqs) {
      auto bytesSkipped = IndexCodec::tfreqCodec.skipBlock(docIS.ptr(), docIS.left());
      docIS.skip(bytesSkipped);
      skipCount(SkipStats::docsOnlyFreqBlocksSkipped);
    }

    docBlockResident = true;
    residentWordsPtr = wordsPtr;
    residentDocBase = docBase;
    residentBlockLast = blockLast;
    residentBlockStartOrd = blockStartOrd;
    residentNumWords = numWords;
    db[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) blockLast;
    docBufEnd = Postings::DOCS_BLOCK_SIZE;
    nextL0Base = blockLast;
    nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
  }

  bool advanceResident(int32_t target) {
    assert(docBlockResident);
    int32_t landing = 0;
    int32_t ordAfter = 0;
    if (!findResidentGEQ(target, landing, ordAfter)) {
      finishResidentBlock();
      return false;
    }
    positionResidentAt(landing, ordAfter, false);
    skipCount(SkipStats::docsOnlyWordProbeAdvances);
    return true;
  }

  bool tryEnterResidentBlock(int32_t target) {
    assert(!docBlockResident);
    if (docfreq - docOrd < Postings::DOCS_BLOCK_SIZE) {
      return false;
    }
    const int32_t blockStartOrd = docOrd;
    const uint32_t base = docOrd == 0 ? 0 : (uint32_t) db[Postings::DOCS_BLOCK_SIZE - 1];
    const uint32_t docBase = base + (blockStartOrd == 0 ? 0 : 1);

    seekToBlockBody();
    bodyReady = true;
    const int8_t token = (int8_t) *docIS.ptr();
    if (token > 0) {
      return false;
    }
    const int32_t numWords = token == Postings::DOC_BLOCK_CONTIGUOUS ? 0 : -token;
    const char* wordsPtr = docIS.ptr() + 1;
    uint32_t blockLast;
    if (numWords == 0) {
      blockLast = docBase + Postings::DOCS_BLOCK_SIZE - 1;
    } else {
      assert(numWords > 0);
      uint64_t lastWord = loadWord64(wordsPtr + (int64_t) (numWords - 1) * 8);
      assert(lastWord != 0);
      blockLast = docBase + (uint32_t) (numWords - 1) * 64
                  + (63 - (uint32_t) std::countl_zero(lastWord));
    }
    enterResidentBlock(blockStartOrd, docBase, blockLast, numWords);
    return advanceResident(target);
  }

  static void orBitRange(std::span<uint64_t> bits, int32_t firstIndex,
                         int32_t count) {
    assert(firstIndex >= 0);
    assert(count >= 0);
    int32_t word = firstIndex >> 6;
    int32_t bit = firstIndex & 63;
    while (count > 0) {
      int32_t take = std::min(count, 64 - bit);
      uint64_t mask = take == 64 ? ~0ULL : ((1ULL << take) - 1ULL) << bit;
      bits[(size_t) word] |= mask;
      count -= take;
      word++;
      bit = 0;
    }
  }

  static void orDocBits(std::span<uint64_t> bits, const int32_t* docs,
                        int32_t count, int32_t bitsBase) {
    if (count <= 0) {
      return;
    }
    if (docs[count - 1] - docs[0] == count - 1) {
      skipCount(SkipStats::countBulkFillContiguousBlocks);
      orBitRange(bits, docs[0] - bitsBase, count);
      return;
    }
    for (int32_t i = 0; i < count; i++) {
      int32_t index = docs[i] - bitsBase;
      bits[(size_t) (index >> 6)] |= 1ULL << (index & 63);
    }
  }

  static void orShiftedWords(std::span<uint64_t> bits, int32_t bitsBase,
                             const char* src, int32_t numWords,
                             uint32_t docBase, int32_t clipDoc) {
    assert(clipDoc >= bitsBase);
    for (int32_t w = 0; w < numWords; w++) {
      uint64_t word;
      memcpy(&word, src + (int64_t) w * 8, 8);
      if (word == 0) {
        continue;
      }
      const int64_t wordDoc0 = (int64_t) docBase + ((int64_t) w << 6);
      if (wordDoc0 + 63 < clipDoc) {
        continue;
      }
      if (clipDoc > wordDoc0) {
        word &= ~0ULL << (clipDoc - wordDoc0);
        if (word == 0) {
          continue;
        }
      }
      int64_t dstBit0 = wordDoc0 - bitsBase;
      if (dstBit0 < 0) {
        word >>= (uint32_t) -dstBit0;
        dstBit0 = 0;
      }
      const size_t idx = (size_t) (dstBit0 >> 6);
      const int32_t off = (int32_t) (dstBit0 & 63);
      bits[idx] |= word << off;
      if (off != 0) {
        uint64_t hi = word >> (64 - off);
        if (hi != 0) {
          bits[idx + 1] |= hi;
        }
      }
    }
  }

  static void orShiftedWordsRange(std::span<uint64_t> bits, int32_t bitsBase,
                                  const char* src, int32_t numWords,
                                  uint32_t docBase, int32_t clipDoc,
                                  int32_t upTo) {
    assert(clipDoc >= bitsBase);
    assert(upTo >= clipDoc);
    for (int32_t w = 0; w < numWords; w++) {
      uint64_t word;
      memcpy(&word, src + (int64_t) w * 8, 8);
      if (word == 0) {
        continue;
      }
      const int64_t wordDoc0 = (int64_t) docBase + ((int64_t) w << 6);
      if (wordDoc0 + 63 < clipDoc) {
        continue;
      }
      if (wordDoc0 >= upTo) {
        break;
      }
      int32_t lo = clipDoc > wordDoc0 ? clipDoc - (int32_t) wordDoc0 : 0;
      int32_t hi = (int64_t) upTo < wordDoc0 + 64
                   ? upTo - (int32_t) wordDoc0 : 64;
      if (lo >= hi) {
        continue;
      }
      word &= lowBitsMask(hi) & ~lowBitsMask(lo);
      if (word == 0) {
        continue;
      }
      int64_t dstBit0 = wordDoc0 - bitsBase;
      if (dstBit0 < 0) {
        word >>= (uint32_t) -dstBit0;
        dstBit0 = 0;
      }
      const size_t idx = (size_t) (dstBit0 >> 6);
      const int32_t off = (int32_t) (dstBit0 & 63);
      bits[idx] |= word << off;
      if (off != 0) {
        uint64_t hiWord = word >> (64 - off);
        if (hiWord != 0) {
          bits[idx + 1] |= hiWord;
        }
      }
    }
  }

  bool orResidentIntoBitSet(std::span<uint64_t> bits, int32_t bitsBase,
                            int32_t upTo) {
    assert(docBlockResident);
    const int32_t minDoc = docid < 0 ? (int32_t) residentDocBase : docid;
    if (upTo <= minDoc) {
      return false;
    }
    int32_t lastDoc = 0;
    int32_t ordAfter = 0;
    if (!findResidentLastBefore(upTo, minDoc, lastDoc, ordAfter)) {
      return false;
    }
    const int32_t clipDoc = std::max(minDoc, bitsBase);
    const int32_t emitTo = std::min(upTo, (int32_t) residentBlockLast + 1);
    if (clipDoc < emitTo) {
      if (residentNumWords == 0) {
        orBitRange(bits, clipDoc - bitsBase, emitTo - clipDoc);
      } else {
        orShiftedWordsRange(bits, bitsBase, residentWordsPtr, residentNumWords,
                            residentDocBase, clipDoc, emitTo);
      }
      skipCount(SkipStats::countBulkFillWordBlocks);
    }
    if (lastDoc == (int32_t) residentBlockLast) {
      finishResidentBlock();
      return true;
    }
    positionResidentAt(lastDoc, ordAfter, true);
    return false;
  }

  bool orWholeWordBlocks(std::span<uint64_t> bits, int32_t bitsBase,
                         int32_t upTo) {
    bool consumed = false;
    while (docfreq - docOrd >= Postings::DOCS_BLOCK_SIZE) {
      seekToBlockBody();
      bodyReady = true;
      const int8_t token = (int8_t) *docIS.ptr();
      if (token > 0) {
        break;
      }
      const uint32_t base = docOrd == 0 ? 0 : (uint32_t) db[Postings::DOCS_BLOCK_SIZE - 1];
      const uint32_t docBase = base + (docOrd == 0 ? 0 : 1);
      const int32_t numWords = token == Postings::DOC_BLOCK_CONTIGUOUS ? 0 : -token;
      uint32_t blockLast;
      if (numWords == 0) {
        blockLast = docBase + Postings::DOCS_BLOCK_SIZE - 1;
      } else {
        uint64_t lastWord = loadWord64(docIS.ptr() + 1 + (int64_t) (numWords - 1) * 8);
        assert(lastWord != 0);
        blockLast = docBase + (uint32_t) (numWords - 1) * 64
                    + (63 - (uint32_t) std::countl_zero(lastWord));
      }
      if ((int32_t) docBase >= upTo) {
        break;
      }
      if ((int32_t) blockLast >= upTo) {
        enterResidentBlock(docOrd, docBase, blockLast, numWords);
        consumed = true;
        break;
      }

      const int32_t clipDoc = std::max((int32_t) docBase, bitsBase);
      if (numWords == 0) {
        if ((int32_t) blockLast >= clipDoc) {
          orBitRange(bits, clipDoc - bitsBase, (int32_t) blockLast - clipDoc + 1);
        }
      } else {
        orShiftedWords(bits, bitsBase, docIS.ptr() + 1, numWords, docBase, clipDoc);
      }
      skipCount(SkipStats::countBulkFillWordBlocks);

      docIS.skip(1 + (int64_t) numWords * 8);
      if (hasFreqs) {
        auto bytesSkipped = IndexCodec::tfreqCodec.skipBlock(docIS.ptr(), docIS.left());
        docIS.skip(bytesSkipped);
        skipCount(SkipStats::docsOnlyFreqBlocksSkipped);
      }
      bodyReady = false;
      const int32_t blockStartOrd = docOrd;
      docOrd += Postings::DOCS_BLOCK_SIZE;
      docid = (int32_t) blockLast;
      blockMode = true;
      db[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) blockLast;
      docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
      nextL0Base = blockLast;
      nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
      consumed = true;
    }
    return consumed;
  }

  static int32_t popcountRange(std::span<const uint64_t> bits,
                               int32_t firstIndex, int32_t count) {
    assert(firstIndex >= 0);
    assert(count >= 0);
    int32_t word = firstIndex >> 6;
    int32_t bit = firstIndex & 63;
    int32_t total = 0;
    while (count > 0) {
      int32_t take = std::min(count, 64 - bit);
      uint64_t mask = take == 64 ? ~0ULL : ((1ULL << take) - 1ULL) << bit;
      total += (int32_t) std::popcount(bits[(size_t) word] & mask);
      count -= take;
      word++;
      bit = 0;
    }
    return total;
  }

  // popcount(stored block words AND bits), where stored bit i = doc docBase+i
  // and `bits` bit j = doc j. A stored word's set bits never pass the block's
  // last doc, so only in-range `bits` words are ever consulted.
  static int32_t andPopcountShiftedWords(std::span<const uint64_t> bits,
                                         const char* src, int32_t numWords,
                                         uint32_t docBase) {
    const int32_t nBitsWords = (int32_t) bits.size();
    const int32_t baseWord = (int32_t) (docBase >> 6);
    const int32_t off = (int32_t) (docBase & 63);
    int32_t total = 0;
    for (int32_t w = 0; w < numWords; w++) {
      uint64_t word = loadWord64(src + (int64_t) w * 8);
      if (word == 0) {
        continue;
      }
      const int32_t wi = baseWord + w;
      uint64_t chunk = bits[(size_t) wi] >> off;
      if (off != 0 && wi + 1 < nBitsWords) {
        chunk |= bits[(size_t) (wi + 1)] << (64 - off);
      }
      total += (int32_t) std::popcount(word & chunk);
    }
    return total;
  }

  // countInBitSet's whole-block walk: count word/contiguous full blocks by
  // AND+popcount straight from the stream, consuming them wholesale. Stops
  // with the stream ready for the regular decode on a packed block or the
  // tail. Returns whether any block was consumed.
  bool countWholeWordBlocks(std::span<const uint64_t> bits, int32_t& hits) {
    bool consumed = false;
    while (docfreq - docOrd >= Postings::DOCS_BLOCK_SIZE) {
      seekToBlockBody();
      bodyReady = true;
      const int8_t token = (int8_t) *docIS.ptr();
      if (token > 0) {
        break;  // packed: the regular decode path takes it from here
      }
      const uint32_t base = docOrd == 0 ? 0 : (uint32_t) db[Postings::DOCS_BLOCK_SIZE - 1];
      const uint32_t docBase = base + (docOrd == 0 ? 0 : 1);
      const int32_t numWords = token == Postings::DOC_BLOCK_CONTIGUOUS ? 0 : -token;
      uint32_t blockLast;
      if (numWords == 0) {
        blockLast = docBase + Postings::DOCS_BLOCK_SIZE - 1;
        hits += popcountRange(bits, (int32_t) docBase, Postings::DOCS_BLOCK_SIZE);
        skipCount(SkipStats::countBulkFillContiguousBlocks);
      } else {
        uint64_t lastWord = loadWord64(docIS.ptr() + 1 + (int64_t) (numWords - 1) * 8);
        assert(lastWord != 0);
        blockLast = docBase + (uint32_t) (numWords - 1) * 64
                    + (63 - (uint32_t) std::countl_zero(lastWord));
        hits += andPopcountShiftedWords(bits, docIS.ptr() + 1, numWords, docBase);
      }
      skipCount(SkipStats::countBulkFillWordBlocks);

      docIS.skip(1 + (int64_t) numWords * 8);
      if (hasFreqs) {
        auto bytesSkipped = IndexCodec::tfreqCodec.skipBlock(docIS.ptr(), docIS.left());
        docIS.skip(bytesSkipped);
        skipCount(SkipStats::docsOnlyFreqBlocksSkipped);
      }
      bodyReady = false;
      const int32_t blockStartOrd = docOrd;
      docOrd += Postings::DOCS_BLOCK_SIZE;
      docid = (int32_t) blockLast;
      blockMode = true;
      db[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) blockLast;
      docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
      nextL0Base = blockLast;
      nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
      consumed = true;
    }
    return consumed;
  }

  void skipToBlock(int32_t target) {
    assert(!docBlockResident);
    blockMode = false;
    const char* const streamStart = docIS.ptr(0);
    const char* const end = docIS.ptr(endOfDocs);
    const char* p = docIS.ptr();
    int32_t block = nextL0Block;
    uint32_t prevLastDoc = nextL0Base;

    auto walkL0To = [&](int32_t maxBlock) -> bool {
      while (block < maxBlock && block < numDocBlocks) {
        skipCount(SkipStats::l0HeaderSteps);
        uint32_t headerLen = InputStream::readVint(p, end);
        const char* headerEnd = p + headerLen;
        assert(headerEnd <= end);
        uint32_t blockLastDoc = prevLastDoc + readVint15(p, headerEnd);
        uint64_t blockByteLen = readVlong15(p, headerEnd);
        const char* body = headerEnd;
        if (target <= (int32_t) blockLastDoc) {
          docIS.seek(body - streamStart);
          docOrd = block * Postings::DOCS_BLOCK_SIZE;
          db[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) prevLastDoc;
          docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;
          nextL0Block = block;
          nextL0Base = prevLastDoc;
          bodyReady = true;
          return true;
        }
        p = body + (int64_t) blockByteLen;
        assert(p <= end);
        prevLastDoc = blockLastDoc;
        block++;
      }
      return false;
    };

    if (!isL1Boundary(block)) {
      int32_t groupStartBlock = (block / L1_PERIOD) * L1_PERIOD;
      if ((int64_t) target - (int64_t) prevLastDoc
              >= L0_CHECKPOINT_MIN_SKIP_DOCS
          && l0CheckpointGroupStartBlock == groupStartBlock) {
        L0Checkpoint checkpoint;
        if (consultL0Checkpoints(
                l0CheckpointEntries, l0CheckpointCount, (uint32_t) target,
                block - groupStartBlock, checkpoint)) {
          p = l0CheckpointGroupBody + checkpoint.bodyOffset;
          block = groupStartBlock + checkpoint.blockIdxInGroup;
          prevLastDoc = checkpoint.key;
          skipCount(SkipStats::l0CheckpointJumps);
        }
      }
      int32_t nextGroupBlock = ((block / L1_PERIOD) + 1) * L1_PERIOD;
      if (walkL0To(nextGroupBlock)) {
        return;
      }
    }

    while (block < numDocBlocks) {
      assert(isL1Boundary(block));
      skipCount(SkipStats::l1GroupSteps);
      uint32_t groupHeaderLen = InputStream::readVint(p, end);
      const char* groupHeaderEnd = p + groupHeaderLen;
      assert(groupHeaderEnd <= end);
      uint32_t groupLastDoc = prevLastDoc + readVint15(p, groupHeaderEnd);
      uint64_t groupByteLen = readVlong15(p, groupHeaderEnd);
      assert(p < groupHeaderEnd);
      p++;  // per-group packed-block count: no cursor consumer
      assert(p < groupHeaderEnd);
      uint8_t entryCount = (uint8_t) *p++;
      const char* checkpointEntries = p;
      assert(p + (size_t) entryCount * sizeof(uint64_t) <= groupHeaderEnd);
      p += (size_t) entryCount * sizeof(uint64_t);
      int32_t groupBlockCount = std::min(L1_PERIOD, numDocBlocks - block);
      int32_t groupStartBlock = block;
      l0CheckpointEntries = checkpointEntries;
      l0CheckpointCount = entryCount;
      l0CheckpointGroupBody = groupHeaderEnd;
      l0CheckpointGroupStartBlock = groupStartBlock;
      const char* groupBody = groupHeaderEnd;
      if (target <= (int32_t) groupLastDoc) {
        p = groupBody;
        L0Checkpoint checkpoint;
        if (consultL0Checkpoints(
                checkpointEntries, entryCount, (uint32_t) target, -1,
                checkpoint)) {
          p = groupBody + checkpoint.bodyOffset;
          assert(p < groupBody + (int64_t) groupByteLen);
          block = groupStartBlock + checkpoint.blockIdxInGroup;
          prevLastDoc = checkpoint.key;
          skipCount(SkipStats::l0CheckpointJumps);
        }
        if (walkL0To(groupStartBlock + groupBlockCount)) {
          return;
        }
        assert(false);
        return;
      }
      p = groupBody + (int64_t) groupByteLen;
      assert(p <= end);
      prevLastDoc = groupLastDoc;
      block += groupBlockCount;
    }

    docIS.seek(p - streamStart);
    docOrd = docfreq;
    docBufIdx = docBufEnd = 0;
    nextL0Block = numDocBlocks;
    nextL0Base = prevLastDoc;
    bodyReady = false;
  }

public:
  static constexpr DocsEnumTier TIER = DocsEnumTier::DOCS;

  explicit BasicDocsEnum(const TermsEnum::PostingsState& state)
      : DocsEnumMeta(state) {
    if (docsSize == 0) {
      assert(docfreq == 1);
      assert(ttf == 1);
      db[0] = state.pulsedDoc;
      docBufEnd = 1;
    } else {
      assert(docIS.offset() == startOfDocs);
    }
  }

  explicit BasicDocsEnum(TermsEnum& termsEnum)
      : BasicDocsEnum(termsEnum.postingsState()) {}

  BasicDocsEnum(const BasicDocsEnum&) = delete;

  // Cheap lower bound on the first doc of a term's postings: no enum
  // construction, no block decode. Exact for pulsed terms and for terms
  // whose docs fit one StreamVByte tail (docFreq < DOCS_BLOCK_SIZE); terms
  // with full blocks return 0, since reaching their first doc would mean
  // unpacking part of a coded block.
  static int32_t firstDocLowerBound(const TermsEnum::PostingsState& state) {
    if (state.docsEnd == state.docsStart) {
      assert(state.pulsedDoc >= 0);
      return state.pulsedDoc;
    }
    if (state.docFreq >= Postings::DOCS_BLOCK_SIZE) {
      return 0;
    }
    // One StreamVByte tail: [group header][L0 header][keys][data], docs d1
    // coded from base 0, so the first coded value is the first doc.
    InputStream is = state.docIS;
    auto groupHeaderLen = is.readVint();
    is.skip(groupHeaderLen);
    auto headerLen = is.readVint();
    is.skip(headerLen);
    const uint8_t* keys = (const uint8_t*) is.ptr();
    const uint8_t* data = keys + svbKeyBytes((uint32_t) state.docFreq);
    const int32_t len = (keys[0] & 3) + 1;
    assert((const char*) (data + len) <= is.ptr() + is.left());
    uint32_t doc = 0;
    for (int32_t i = 0; i < len; i++) {
      doc |= (uint32_t) data[i] << (8 * i);
    }
    return (int32_t) doc;
  }

  int32_t docId() const { return docid; }

  int32_t next() { return nextDoc(); }

  int32_t nextDoc() {
    assert(docid != PostingsReader::END);
    if (docBlockResident) {
      int32_t landing = 0;
      int32_t ordAfter = 0;
      if (findResidentGEQ(docid + 1, landing, ordAfter)) {
        positionResidentAt(landing, ordAfter, false);
        return docid;
      }
      finishResidentBlock();
    }
    blockMode = false;
    if (docBufIdx >= docBufEnd) {
      const int32_t leftToRead = docfreq - docOrd;
      if (leftToRead <= 0) {
        assert(leftToRead == 0);
        docid = PostingsReader::END;
        return docid;
      }

      const uint32_t base = docOrd == 0 ? 0 : (uint32_t) db[Postings::DOCS_BLOCK_SIZE - 1];
      const int32_t blockStartOrd = docOrd;
      seekToBlockBody();
      skipCount(SkipStats::docBlocksDecoded);

      if (leftToRead >= Postings::DOCS_BLOCK_SIZE) {
        const int8_t token = (int8_t) *docIS.ptr();
        docIS.skip(1);
        if (token > 0) {
          uint32_t outSz = Postings::DOCS_BLOCK_SIZE;
          auto bytesRead = IndexCodec::docCodec.decodeBlock(
              docIS.ptr(), docIS.left(), (uint32_t*) db, outSz, base);
          docIS.skip(bytesRead);
          assert(outSz == Postings::DOCS_BLOCK_SIZE);
        } else {
          const uint32_t docBase = base + (blockStartOrd == 0 ? 0 : 1);
          if (token == Postings::DOC_BLOCK_CONTIGUOUS) {
            for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
              db[i] = (int32_t) docBase + i;
            }
          } else {
            expandDocWords(docIS.ptr(), -token, docBase);
            docIS.skip((int64_t) -token * 8);
          }
        }
        docBufIdx = 0;
        docBufEnd = Postings::DOCS_BLOCK_SIZE;
        if (hasFreqs) {
          auto bytesSkipped = IndexCodec::tfreqCodec.skipBlock(docIS.ptr(), docIS.left());
          docIS.skip(bytesSkipped);
          skipCount(SkipStats::docsOnlyFreqBlocksSkipped);
        }
      } else {
        const uint32_t n = (uint32_t) leftToRead;
        const uint32_t keyBytes = svbKeyBytes(n);
        uint8_t* p = (uint8_t*) docIS.ptr();
        uint8_t* dataEnd = svb_decode_avx_d1_init((uint32_t*) db, p, p + keyBytes,
                                                  n, base);
        if (hasFreqs) {
          dataEnd += svbEncodedBytes(dataEnd, n);
        }
        docIS.skip(dataEnd - p);
        docBufIdx = 0;
        docBufEnd = leftToRead;
      }

      nextL0Base = (uint32_t) db[docBufEnd - 1];
      nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
    }

    docid = db[docBufIdx++];
    docOrd++;
    return docid;
  }

  std::span<const int32_t> peekDocBlock() {
    if (docid == PostingsReader::END) {
      return {};
    }
    materializeResidentBlock();
    int32_t start = 0;
    if (!blockMode) {
      if (docid < 0 && nextDoc() == PostingsReader::END) {
        return {};
      }
      start = docBufIdx - 1;
    } else if (docBufIdx >= docBufEnd) {
      if (nextDoc() == PostingsReader::END) {
        return {};
      }
      start = docBufIdx - 1;
    } else {
      start = docBufIdx;
    }
    if (start >= docBufEnd) {
      return {};
    }
    return std::span<const int32_t>(db + start, (size_t) (docBufEnd - start));
  }

  void consumeDocBlock(int32_t n) {
    assert(n >= 0);
    if (n == 0) {
      return;
    }
    assert(docid != PostingsReader::END);
    materializeResidentBlock();
    int32_t start = blockMode ? docBufIdx : docBufIdx - 1;
    const bool countedCurrent = !blockMode;
    assert(!countedCurrent || docid >= 0);
    const int32_t limit = start + n;
    assert(start >= 0 && limit <= docBufEnd);
    const int32_t newlyCounted = n - (countedCurrent ? 1 : 0);
    assert(newlyCounted >= 0);
    docOrd += newlyCounted;
    docBufIdx = limit;
    docid = db[limit - 1];
    blockMode = true;
  }

  void intoBitSet(std::span<uint64_t> bits, int32_t bitsBase, int32_t upTo) {
    for (;;) {
      if (docBlockResident) {
        if (orResidentIntoBitSet(bits, bitsBase, upTo)) {
          continue;
        }
        return;
      }
      if ((docid < 0 || blockMode) && docBufIdx >= docBufEnd
          && orWholeWordBlocks(bits, bitsBase, upTo)) {
        continue;
      }
      auto blockDocs = peekDocBlock();
      int32_t available = (int32_t) blockDocs.size();
      if (available == 0) {
        return;
      }
      int32_t used = 0;
      while (used < available && blockDocs[(size_t) used] < bitsBase) {
        used++;
      }
      int32_t firstEmit = used;
      while (used < available && blockDocs[(size_t) used] < upTo) {
        used++;
      }
      int32_t emit = used - firstEmit;
      if (emit > 0) {
        skipCount(SkipStats::countBulkFillBlocks);
        if (SkipStats::enabled) {
          SkipStats::countBulkFillDocs += emit;
        }
        orDocBits(bits, blockDocs.data() + firstEmit, emit, bitsBase);
      }
      if (used == 0) {
        return;
      }
      consumeDocBlock(used);
      if (used < available) {
        return;
      }
    }
  }

  // Count every remaining doc (including a currently-positioned one, like
  // intoBitSet) whose bit is set in `bits` (bit index == doc id, sized to
  // cover [0, maxDoc)), consuming the enum. intoBitSet's walk with counting
  // in place of production: word and contiguous full blocks count by
  // AND+popcount straight from the stream without expanding doc ids; packed
  // blocks and the tail decode and probe.
  int32_t countInBitSet(std::span<const uint64_t> bits) {
    int32_t hits = 0;
    materializeResidentBlock();
    for (;;) {
      if ((docid < 0 || blockMode) && docBufIdx >= docBufEnd
          && countWholeWordBlocks(bits, hits)) {
        continue;
      }
      auto blockDocs = peekDocBlock();
      int32_t available = (int32_t) blockDocs.size();
      if (available == 0) {
        return hits;
      }
      skipCount(SkipStats::countBulkFillBlocks);
      if (SkipStats::enabled) {
        SkipStats::countBulkFillDocs += available;
      }
      for (int32_t i = 0; i < available; i++) {
        int32_t doc = blockDocs[(size_t) i];
        hits += (int32_t) ((bits[(size_t) (doc >> 6)] >> (doc & 63)) & 1);
      }
      consumeDocBlock(available);
    }
  }

  int32_t advance(int32_t target) {
    assert(docid < target);
    skipCount(SkipStats::advanceCalls);
    if (docBlockResident && advanceResident(target)) {
      return docid;
    }
    if (nextL0Block < numDocBlocks
        && (docBufEnd == 0 || target > db[docBufEnd - 1])) {
      skipToBlock(target);
    }
    if (docBufIdx >= docBufEnd && tryEnterResidentBlock(target)) {
      return docid;
    }
    if (docBufIdx >= docBufEnd && nextDoc() >= target) {
      return docid;
    }
    if (db[docBufEnd - 1] < target) {
      while (docid < target) {
        nextDoc();
      }
      return docid;
    }
    blockMode = false;
    const int32_t start = docBufIdx;
    const int32_t j =
        DecodedSuccessor::index(db, start, docBufEnd, target);
    assert(j < docBufEnd);
    docOrd += j + 1 - start;
    docBufIdx = j + 1;
    docid = db[j];
    return docid;
  }
};

template<>
class BasicDocsEnum<DocsEnumTier::FREQS> final : public DocsEnumImpl {
public:
  static constexpr DocsEnumTier TIER = DocsEnumTier::FREQS;

  explicit BasicDocsEnum(const TermsEnum::PostingsState& state)
      : DocsEnumImpl(state) {}
  explicit BasicDocsEnum(TermsEnum& termsEnum) : DocsEnumImpl(termsEnum) {}

  BasicDocsEnum(const BasicDocsEnum&) = delete;
};

template<>
class BasicDocsEnum<DocsEnumTier::POSITIONS> final : public DocsEnumImpl {
  using DocsEnumImpl::advanceDocOnly;
  using DocsEnumImpl::consumeDocOnlyBlock;
  using DocsEnumImpl::intoBitSet;
  using DocsEnumImpl::nextDocOnly;
  using DocsEnumImpl::peekDocBlock;
  using DocsEnumImpl::peekDocOnlyBlock;

public:
  static constexpr DocsEnumTier TIER = DocsEnumTier::POSITIONS;

  explicit BasicDocsEnum(const TermsEnum::PostingsState& state)
      : DocsEnumImpl(state) {}
  explicit BasicDocsEnum(TermsEnum& termsEnum) : DocsEnumImpl(termsEnum) {}

  BasicDocsEnum(const BasicDocsEnum&) = delete;
};

// Each dense cursor keeps the current group's two directory pointers and its
// compact group identity, adding 24 bytes.
static_assert(sizeof(DocsEnumMeta) == 96);
static_assert(sizeof(DocsEnumImpl) == 1400);
static_assert(sizeof(DocsEnumMeta) < sizeof(DocsEnumImpl));
static_assert(sizeof(DocsOnlyEnum) == 720);
static_assert(sizeof(DocsOnlyEnum) < sizeof(DocsEnumImpl));
static_assert(sizeof(DocsFreqEnum) == sizeof(DocsEnumImpl));
static_assert(sizeof(DocsPosEnum) == sizeof(DocsEnumImpl));
static_assert(std::is_trivially_destructible_v<DocsEnumMeta>);
static_assert(std::is_trivially_destructible_v<DocsEnumImpl>);
static_assert(std::is_trivially_destructible_v<DocsOnlyEnum>);
static_assert(std::is_trivially_destructible_v<DocsFreqEnum>);
static_assert(std::is_trivially_destructible_v<DocsPosEnum>);

}
