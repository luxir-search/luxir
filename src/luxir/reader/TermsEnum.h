// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
#include "luxir/codec/StreamVByte.h"
#include "PostingsReader.h"
#include "Postings.h"
#include "SkipStats.h"
#include "TrieReader.h"
#include "luxir/util/screaming.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace luxir {
// Seeks and enumerates the terms of one field, loading postings metadata on demand.
class TermsEnum {
  friend class DocsEnum;
  friend class ScanTermsEnum;

  InputStream termsIS;  // Dictionary blocks, offsets and trie for this field.
  SegmentCore& segmentCore;

  const field_parts::Terms* termData;  // Null for a field without a dictionary.
  const field_parts::IndexedTotals& totals;  // Field-wide indexed counts.
  FieldType::flag_type flags;

  // currTerm's buffer holds a full PackedTerm plus COPY_SLACK, and the index
  // files are padded past every term block, so a short copy into currTerm may
  // move a fixed COPY_SLACK bytes: gcc inlines that as one vector move, with no
  // libc call.
  static constexpr size_t COPY_SLACK = 16;
  static constexpr size_t TERM_BUFFER_BYTES =
      PackedTerm::getMemSize(PackedTerm::MAX_BYTES) + COPY_SLACK;
  void copyIntoTerm(char* dst, const char* src, size_t len) {
    assert(dst >= (char*) currTerm.ptr()
           && dst + std::max(len, COPY_SLACK) <= (char*) currTerm.ptr() + TERM_BUFFER_BYTES);
    if (len <= COPY_SLACK) memcpy(dst, src, COPY_SLACK);
    else memcpy(dst, src, len);
  }

  PackedTerm currTerm;  // Buffer for the reconstructed current term.
  int32_t ordInBlock = -1; // the term number local to the current block
  // Bytes currTerm shares with the term immediately before it, in full-term
  // coordinates.  Only an in-block advance can report a nonzero value; see
  // sharedPrefixLen().
  int32_t sharedLen = 0;

  // Information for the current term block.

  PackedTerm startingTerm = PackedTerm(nullptr);  // First term in the loaded block.
  int32_t termBlockIndex = -1; // what term block are we currently in
  int64_t startingOrd = 0;  // Ordinal of the block's first term.
  int32_t maxOrdInBlock = -1;  // Local ordinal of the last term.
  int64_t locOfDocsForTermBlock = 0;  // Offset within the run's docs stream.
  int64_t locOfPositionsForTermBlock = 0;  // Offset within the run's positions stream.
  const char* termHashes = nullptr;  // One hash byte per term in the block.
  uint8_t blockPrefixLen = 0;
  uint32_t pulsedMask = 0;  // One bit per term with inline postings.

  const char* prefixLens = nullptr;
  const char* suffixLens = nullptr;
  const char* suffixBlob = nullptr;
  // Start of the suffix bytes for the next in-block advance.  Suffixes are
  // stored back to back in dictionary order and every in-block movement steps
  // exactly one ord, so a running cursor replaces a prefix-summed start table.
  const char* suffixCursor = nullptr;
  const char* metadataRuns = nullptr;  // Start of the metadata run lengths.
  const char* blockEnd = nullptr;  // End of the encoded block.
  uint32_t suffixBytesTotal = 0;

  const char* docsEndRun = nullptr;
  const char* packedBlocksRun = nullptr;
  const char* dfRun = nullptr;
  const char* ttfCodeRun = nullptr;
  const char* posOffRun = nullptr;
  const char* termImpactRun = nullptr;
  const char* pulsedRun = nullptr;
  uint32_t docsEndRunLen = 0;
  uint32_t packedBlocksRunLen = 0;
  uint32_t dfRunLen = 0;
  uint32_t ttfCodeRunLen = 0;
  uint32_t posOffRunLen = 0;
  uint32_t termImpactRunLen = 0;
  uint32_t pulsedRunLen = 0;
  // Start of term impactCursorOrd's frontier in termImpactRun, so in-order
  // access skips one frontier per term instead of all earlier ones.
  const char* impactCursor = nullptr;
  int32_t impactCursorOrd = 0;

  std::array<uint64_t, Postings::TERMS_BLOCK_SIZE> docsEnds{};
  std::array<uint32_t, Postings::TERMS_BLOCK_SIZE> packedBlockCounts{};
  std::array<uint32_t, Postings::TERMS_BLOCK_SIZE> docFreqs{};
  std::array<uint64_t, Postings::TERMS_BLOCK_SIZE> ttfCodes{};
  std::array<uint64_t, Postings::TERMS_BLOCK_SIZE> posOffsets{};
  std::array<uint32_t, Postings::TERMS_BLOCK_SIZE * 2> pulsedValues{};
  bool statsDecoded = false;
  bool postingsDecoded = false;
  bool metadataRunsParsed = false;

  // Dictionary routing data.
  const field_parts::Terms::BlockOffset* termBlockOffsets = nullptr;
  const char* trieBase = nullptr;  // Start of the trie bytes.
  int32_t numTermBlocks = 0;
  const field_parts::TermRun* runs = nullptr;
  const field_parts::TermRun* run = nullptr;  // The selected dictionary run.
  int32_t runCount = 0;
  int64_t runLastTermOrd = -1;  // Last term ordinal in the current run.
  int32_t runEndBlockOrd = 0;  // one past the current run's last block ord
  // Cached offsets within termsIS for the current run.
  int64_t runTermsBase = 0;
  int64_t runTermsEnd = 0;
  InputStream runDocs;
  InputStream runPositions;

public:
  struct BlockEstimate {
    std::string firstTerm;
    uint64_t bytes;
  };

  struct EncodedImpactFrontier {
    const char* ptr = nullptr;
    uint32_t len = 0;
  };

  // Immutable state captured from a positioned TermsEnum.  It is sufficient
  // to construct independent DocsEnums without retaining this mutable enum or
  // seeking the term dictionary again.
  struct PostingsState {
    InputStream docIS;
    InputStream posIS;
    EncodedImpactFrontier termImpactFrontier;
    // Offsets are relative to docIS and posIS, respectively.
    int64_t docsStart = 0;
    int64_t docsEnd = 0;
    int64_t posStart = 0;
    int64_t totalTermFreq = 0;
    int32_t docFreq = 0;
    int32_t packedBlockCount = 0;
    int64_t termOrdinal = -1;
    int32_t pulsedDoc = -1;
    int32_t pulsedPos = -1;
    bool hasFreqs = false;
    bool hasPositions = false;
  };

  // The decoded record and segment core must outlive the enum.
  TermsEnum(MemPool& pool, PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : segmentCore(reader.core()), termData(fieldInfo.terms()), totals(fieldInfo.indexedTotals()),
        flags(fieldInfo.flags), currTerm(pool.alloc(TERM_BUFFER_BYTES), 0) {
    if (!termData) return;
    auto blockOffsets = fieldInfo.termBlockOffsets();
    auto trie = field_parts::termTrie(blockOffsets, termData->trieBytes);
    auto start = termData->location.offset();
    termsIS = streamThrough({termData->location, trie.end().offset() - start}, ReadClass::METADATA);
    termBlockOffsets = reinterpret_cast<const field_parts::Terms::BlockOffset*>(
        termsIS.ptr(blockOffsets.location.offset() - start));
    numTermBlocks = (int32_t)(blockOffsets.bytes / sizeof(field_parts::Terms::BlockOffset));
    trieBase = termsIS.ptr(trie.location.offset() - start);
    runCount = (int32_t)termData->runs.size();
    runs = termData->runs.data();
    selectRun(runs);
  }

  int64_t numTerms() const {
    return totals.numTerms;
  }

  /// sumDocFreq is a field-level stat:
  /// The docFreq of a term is the number of documents it appears in. sumDocFreq is the sum across all terms in this field.
  /// If sumDocFreq() equals the record's docsWithField, every doc contains only one term (possibly repeated). May be
  /// useful for detecting single-valued fields even if they were not marked as such.
  int64_t sumDocFreq() const {
    return totals.sumDocFreq;
  }

  /// sumTotalTermFreq is a field-level stat:
  /// termFreq is the number of times the term appears in a single document.
  /// totalTermFreq is the number of times the term appears across all documents in this segment.
  /// sumTotalTermFreq is the sum of totalTermFreq for all terms in this field (i.e. number of tokens indexed)
  int64_t sumTotalTermFreq() const {
    return totals.sumTotalTermFreq;
  }

  int32_t numBlocks() const {
    return numTermBlocks;
  }

  // Walk local term ordinals and docFreqs without reconstructing term bytes.
  // Stats are bulk-decoded once per term block; the suffix blob is never read.
  // ordInBlock is moved directly so the callback can open postings for the
  // term it is handed (see StrFacetOp), but term() stays on the
  // block's first term and the suffix cursor stays at the start of its blob.
  // The enum is therefore left in a state where only ord-indexed accessors are
  // meaningful: callers must not read term() afterwards, and must re-enter
  // through a seek (every one of which reloads a block) before advancing.
  template <class F>
  void forEachDocFreq(F&& callback) {
    for (termBlockIndex = 0; termBlockIndex < numTermBlocks; termBlockIndex++) {
      readTermBlock();
      decodeStats();
      for (int32_t i = 0; i <= maxOrdInBlock; i++) {
        ordInBlock = i;
        callback(ord(), (int32_t)docFreqs[(size_t)ordInBlock]);
      }
    }
  }

  int64_t dictionaryBytes() const {
    return termData ? (int64_t)termData->dictionaryBytes : 0;
  }

  // The next run starts at this run's block end, or the field ends there.
  static int32_t runEndBlock(std::span<const field_parts::TermRun> runs, int32_t run, int32_t blocks) noexcept {
    return run + 1 < (int32_t)runs.size() ? runs[run + 1].firstBlockOrd : blocks;
  }

  // Each run contributes samples at its own stride, starting at its first block.
  static int32_t sampleCount(std::span<const field_parts::TermRun> runs, int32_t blocks, int32_t stride) noexcept {
    int32_t count = 0;
    for (int32_t run = 0; run < (int32_t)runs.size(); ++run) {
      count += 1 + (runEndBlock(runs, run, blocks) - runs[run].firstBlockOrd - 1) / stride;
    }
    return count;
  }

  // One estimate per stride of term blocks. The next sample's offsets, or
  // the exact ends of this run's streams, delimit the sampled postings.
  std::vector<BlockEstimate> estimateTermBlocks(int32_t stride = 1) const {
    assert(stride >= 1);
    std::vector<BlockEstimate> blocks;
    if (!termData) return blocks;
    blocks.reserve((size_t)sampleCount(termData->runs, numTermBlocks, stride));
    for (int32_t runOrd = 0; runOrd < runCount; runOrd++) {
      const auto& sourceRun = runs[runOrd];
      int32_t firstBlock = sourceRun.firstBlockOrd;
      int32_t endBlock = runEndBlock(termData->runs, runOrd, numTermBlocks);
      auto sourceBase = (int64_t)sourceRun.dictionaryStart;
      int32_t count = endBlock - firstBlock;
      assert(count > 0);
      int32_t samples = (count - 1) / stride + 1;

      std::vector<uint64_t> docsOffsets((size_t) samples);
      std::vector<uint64_t> posOffsets((size_t) samples);
      InputStream input = termsIS;
      size_t outputBase = blocks.size();
      for (int32_t i = 0; i < samples; i++) {
        int32_t blockOrd = firstBlock + i * stride;
        input.seek(sourceBase + termBlockOffsets[blockOrd]);
        PackedTerm first = input.readPackedTerm();
        blocks.push_back({std::string(first.data(), first.size()), 0});
        docsOffsets[(size_t) i] = input.readVlong();
        posOffsets[(size_t) i] = input.readVlong();
      }

      for (int32_t i = 0; i < samples; i++) {
        uint64_t docsEnd = i + 1 < samples ? docsOffsets[(size_t)i + 1]
            : sourceRun.docs.bytes;
        uint64_t posEnd = i + 1 < samples ? posOffsets[(size_t)i + 1]
            : sourceRun.positions.bytes;
        assert(docsEnd >= docsOffsets[(size_t)i] && posEnd >= posOffsets[(size_t)i]);
        blocks[outputBase + (size_t)i].bytes = docsEnd - docsOffsets[(size_t)i]
            + posEnd - posOffsets[(size_t)i];
      }
    }
    assert(stride > 1 || (int32_t) blocks.size() == numTermBlocks);
    return blocks;
  }

  /// returns the 0-based ordinal of the current term.
  int64_t ord() const {
    return startingOrd + ordInBlock;
  }

  /// NOTE: the returned term is invalidated/changed if this TermsEnum is moved off this
  /// term (i.e. the moment next() or seek() is called). Make a copy if you wish to keep it!
  /// If called before nextTerm() or seek() is done, returns a 0 length term.
  PackedTerm term() const {
    return currTerm;
  }

  /// The first 16 bytes of term() as a big-endian integer, zero padded: where
  /// two keys differ they order the terms, and equal keys need a full compare.
  /// A short term's load runs into the buffer's COPY_SLACK.
  unsigned __int128 termPrefixKey() const {
    const char* data = currTerm.data();
    uint64_t hi, lo;
    memcpy(&hi, data, 8);
    memcpy(&lo, data + 8, 8);
    auto key = ((unsigned __int128) std::byteswap(hi) << 64) | std::byteswap(lo);
    uint32_t len = currTerm.size();
    if (len < 16) key &= ~(~(unsigned __int128) 0 >> (8 * len));
    return key;
  }

  /// Byte length of term().  Same value as term().size(), but read from a
  /// member rather than from the length byte the term reconstruction just
  /// wrote, so per-term consumers do not pay a store-to-load round trip
  /// through the term buffer.

  /// Number of leading bytes term() shares with the term immediately preceding
  /// it in the dictionary, in full-term coordinates.  The block encoding stores
  /// this, so scanners that compare consecutive terms never have to re-derive
  /// it.  Valid after any successful positioning call.
  ///
  /// It is exact after an advance inside a term block and 0 everywhere else
  /// (block entry, every seek): 0 claims no sharing, so it is always safe.
  /// Callers must only read it as sharing with the term they last looked at
  /// when they advanced one term to get here - after a seek the preceding
  /// dictionary term is not the term they last saw, and its shared length
  /// would be an over-estimate.
  int32_t sharedPrefixLen() const {
    return sharedLen;
  }

  int32_t docFreq() {
    assert(ordInBlock >= 0);
    decodeStats();
    return (int32_t) docFreqs[(size_t) ordInBlock];
  }

  int64_t totalTermFreq() {
    assert(ordInBlock >= 0);
    decodeStats();
    uint64_t ttf = docFreqs[(size_t) ordInBlock];
    if (FieldType::hasFreqs(flags)) {
      ttf += ttfCodes[(size_t) ordInBlock];
    }
    assert(ttf <= INT64_MAX);
    return (int64_t) ttf;
  }

  int32_t packedBlockCount() {
    assert(ordInBlock >= 0);
    decodePostings();
    assert(packedBlockCounts[(size_t) ordInBlock] <= (uint32_t) INT32_MAX);
    return (int32_t) packedBlockCounts[(size_t) ordInBlock];
  }

  // The whole-term (norm, maxTf) Pareto frontier stored in the term block
  // metadata (see TextWriter::flushTerms): the source for a scorer's
  // global max score - never derived by walking postings.  Returns the
  // number of frontier points appended to norms/tfs.
  int32_t readTermImpactFrontier(std::vector<int32_t>& norms, std::vector<int32_t>& tfs) {
    norms.resize(0);
    tfs.resize(0);
    EncodedImpactFrontier encoded = currentTermImpactFrontierSpan();
    if (encoded.ptr == nullptr) {
      return 0;
    }
    const char* p = encoded.ptr;
    const char* end = encoded.ptr + encoded.len;
    uint32_t count = InputStream::readVint(p, end);
    int32_t tf = 0;
    for (uint32_t j = 0; j < count; j++) {
      assert(p < end);
      int32_t norm = (int32_t) (uint8_t) *p;
      p++;
      tf += (int32_t) InputStream::readVint(p, end);
      norms.push_back(norm);
      tfs.push_back(tf);
    }
    assert(p <= end);
    return (int32_t) count;
  }

  bool hasTermImpacts() const {
    return FieldType::hasFreqs(flags) && FieldType::hasPositions(flags);
  }

  EncodedImpactFrontier currentTermImpactFrontierSpan() {
    assert(ordInBlock >= 0);
    if (!hasTermImpacts()) {
      return {};
    }
    parseMetadataRuns();
    if (impactCursorOrd > ordInBlock) {
      impactCursor = termImpactRun;
      impactCursorOrd = 0;
    }
    const char* p = impactCursor;
    const char* end = termImpactRun + termImpactRunLen;
    for (int32_t t = impactCursorOrd; t < ordInBlock; t++) {
      skipEncodedImpactFrontier(p, end);
    }
    const char* start = p;
    impactCursor = start;
    impactCursorOrd = ordInBlock;
    skipEncodedImpactFrontier(p, end);
    assert(p <= end);
    return {start, (uint32_t) (p - start)};
  }

  PostingsState postingsState() {
    assert(ordInBlock >= 0);
    decodePostings();

    PostingsState state;
    state.hasFreqs = FieldType::hasFreqs(flags);
    state.hasPositions = FieldType::hasPositions(flags);
    state.termOrdinal = ord();
    state.docFreq = docFreq();
    state.packedBlockCount = packedBlockCount();
    state.totalTermFreq = totalTermFreq();
    state.termImpactFrontier = currentTermImpactFrontierSpan();

    uint64_t startOffset = ordInBlock == 0 ? 0 : docsEnds[(size_t) ordInBlock - 1];
    uint64_t endOffset = docsEnds[(size_t) ordInBlock];
    assert(endOffset >= startOffset);
    assert(startOffset <= INT64_MAX && endOffset <= INT64_MAX);
    state.docsStart = locOfDocsForTermBlock + (int64_t) startOffset;
    state.docsEnd = locOfDocsForTermBlock + (int64_t) endOffset;

    if (state.docsStart == state.docsEnd) {
      assert((pulsedMask & (1u << (uint32_t) ordInBlock)) != 0);
      uint32_t pulsedOrd = std::popcount(
          pulsedMask & ((1u << (uint32_t) ordInBlock) - 1));
      uint32_t valueIndex = pulsedOrd * (state.hasPositions ? 2u : 1u);
      state.pulsedDoc = (int32_t) pulsedValues[valueIndex];
      if (state.hasPositions) {
        state.pulsedPos = (int32_t) pulsedValues[valueIndex + 1];
      }
    } else {
      state.docIS = runDocs;
      state.docIS.seek(state.docsStart);
      if (state.hasPositions) {
        state.posStart = locOfPositionsForTermBlock + (int64_t) posOffsets[(size_t) ordInBlock];
        state.posIS = runPositions;
        state.posIS.seek(state.posStart);
      }
    }
    return state;
  }

protected:
  static void skipEncodedImpactFrontier(const char*& p, const char* end) {
    uint32_t count = InputStream::readVint(p, end);
    for (uint32_t j = 0; j < count; j++) {
      assert(p < end);
      p++;
      InputStream::readVint(p, end);
    }
  }

  uint32_t readMetadataRunLen(const char*& p, const char* end) {
    uint32_t code = InputStream::readVint(p, end);
    assert((code & 1u) == 0);
    return code >> 1u;
  }

  uint32_t blockTermCount() const {
    return (uint32_t) maxOrdInBlock + 1;
  }

  uint32_t validPulsedMask() const {
    uint32_t count = blockTermCount();
    return count == 32 ? UINT32_MAX : ((1u << count) - 1);
  }

  void parseMetadataRuns() {
    if (metadataRunsParsed) {
      return;
    }
    // Metadata run lengths are parsed only when a stats or postings accessor
    // asks for them.  This keeps block entry and pure term scans on the
    // hashes/lengths/suffix path.  See TextWriter::flushTerms for the
    // byte-length code and run order.  A zero byte length marks an all-default
    // run for docsEnd/packedBlocks/df/ttfCode/posOff; pulsedRun still uses
    // zero length only for the existing empty-payload case.
    const char* p = metadataRuns;
    docsEndRunLen = readMetadataRunLen(p, blockEnd);
    packedBlocksRunLen = readMetadataRunLen(p, blockEnd);
    dfRunLen = readMetadataRunLen(p, blockEnd);
    ttfCodeRunLen = FieldType::hasFreqs(flags) ? readMetadataRunLen(p, blockEnd) : 0;
    posOffRunLen = FieldType::hasPositions(flags) ? readMetadataRunLen(p, blockEnd) : 0;
    termImpactRunLen = hasTermImpacts() ? readMetadataRunLen(p, blockEnd) : 0;
    pulsedRunLen = readMetadataRunLen(p, blockEnd);

    docsEndRun = p;
    p += docsEndRunLen;
    assert(p <= blockEnd);
    packedBlocksRun = p;
    p += packedBlocksRunLen;
    assert(p <= blockEnd);
    dfRun = p;
    p += dfRunLen;
    assert(p <= blockEnd);
    if (FieldType::hasFreqs(flags)) {
      ttfCodeRun = p;
      p += ttfCodeRunLen;
      assert(p <= blockEnd);
    } else {
      ttfCodeRun = nullptr;
    }
    if (FieldType::hasPositions(flags)) {
      posOffRun = p;
      p += posOffRunLen;
      assert(p <= blockEnd);
    } else {
      posOffRun = nullptr;
    }
    if (hasTermImpacts()) {
      termImpactRun = p;
      p += termImpactRunLen;
      assert(p <= blockEnd);
    } else {
      termImpactRun = nullptr;
    }
    impactCursor = termImpactRun;
    impactCursorOrd = 0;
    pulsedRun = p;
    p += pulsedRunLen;
    assert(p <= blockEnd);
    metadataRunsParsed = true;
  }

  void decodeSVBRun(const char* run, uint32_t runLen, uint32_t* out, uint32_t count) {
    if (count == 0) {
      assert(runLen == 0);
      return;
    }
    uint32_t keyBytes = svbKeyBytes(count);
    assert(runLen >= keyBytes);
    [[maybe_unused]] uint8_t* dataEnd = svb_decode_avx_simple(out, (uint8_t*) run, (uint8_t*) run + keyBytes, count);
    assert(dataEnd == (uint8_t*) run + runLen);
  }

  void decodeStats() {
    if (statsDecoded) {
      return;
    }
    // Tier 1 metadata: term statistics only.  This decodes df and, for fields
    // with freqs, ttfCode.  It deliberately does not touch docsEnd,
    // packedBlocks, posOff, or pulsed values, so stats-only callers do not pay
    // to open postings.
    parseMetadataRuns();
    uint32_t n = blockTermCount();
    if (dfRunLen == 0) {
      std::fill_n(docFreqs.begin(), n, 1);
    } else {
      decodeSVBRun(dfRun, dfRunLen, docFreqs.data(), n);
    }
    if (FieldType::hasFreqs(flags)) {
      if (ttfCodeRunLen == 0) {
        std::fill_n(ttfCodes.begin(), n, 0);
      } else {
        const char* p = ttfCodeRun;
        const char* end = ttfCodeRun + ttfCodeRunLen;
        for (uint32_t i = 0; i < n; i++) {
          ttfCodes[i] = InputStream::readVlong(p, end);
        }
        assert(p == end);
      }
    }
    statsDecoded = true;
  }

  void decodePostings() {
    if (postingsDecoded) {
      return;
    }
    // Tier 2 metadata: data needed to construct DocsEnum.  docsEnd supplies
    // docs slices, packedBlocks describes their packed codec
    // blocks, posOff supplies positions starts, and pulsedRun supplies inline
    // doc/pos payloads for single-occurrence terms.
    parseMetadataRuns();
    uint32_t n = blockTermCount();
    if (docsEndRunLen == 0) {
      assert((pulsedMask & validPulsedMask()) == validPulsedMask());
      std::fill_n(docsEnds.begin(), n, 0);
    } else {
      const char* p = docsEndRun;
      const char* end = docsEndRun + docsEndRunLen;
      [[maybe_unused]] uint64_t prevDocsEnd = 0;
      for (uint32_t i = 0; i < n; i++) {
        uint64_t docsEnd = InputStream::readVlong(p, end);
        assert(docsEnd >= prevDocsEnd);
        docsEnds[i] = docsEnd;
        prevDocsEnd = docsEnd;
      }
      assert(p == end);
    }

    if (packedBlocksRunLen == 0) {
      std::fill_n(packedBlockCounts.begin(), n, 0);
    } else {
      decodeSVBRun(packedBlocksRun, packedBlocksRunLen, packedBlockCounts.data(), n);
    }

    if (FieldType::hasPositions(flags)) {
      if (posOffRunLen == 0) {
        std::fill_n(posOffsets.begin(), n, 0);
      } else {
        const char* p = posOffRun;
        const char* end = posOffRun + posOffRunLen;
        uint64_t posOffset = 0;
        for (uint32_t i = 0; i < n; i++) {
          posOffset += InputStream::readVlong(p, end);
          posOffsets[i] = posOffset;
        }
        assert(p == end);
      }
    }

    uint32_t pulsedTerms = std::popcount(pulsedMask & validPulsedMask());
    uint32_t pulsedCount = pulsedTerms * (FieldType::hasPositions(flags) ? 2u : 1u);
    decodeSVBRun(pulsedRun, pulsedRunLen, pulsedValues.data(), pulsedCount);
    postingsDecoded = true;
  }

  bool isPulsed() {
    decodePostings();
    return (pulsedMask & (1u << (uint32_t) ordInBlock)) != 0;
  }

  uint64_t docsStartOffset() {
    assert(ordInBlock >= 0);
    decodePostings();
    return ordInBlock == 0 ? 0 : docsEnds[(size_t) ordInBlock - 1];
  }

  uint64_t docsEndOffset() {
    assert(ordInBlock >= 0);
    decodePostings();
    return docsEnds[(size_t) ordInBlock];
  }

  int64_t docsStart() {
    return locOfDocsForTermBlock + (int64_t) docsStartOffset();
  }

  int64_t docsEnd() {
    return locOfDocsForTermBlock + (int64_t) docsEndOffset();
  }

  int64_t docsSize() {
    uint64_t start = docsStartOffset();
    uint64_t end = docsEndOffset();
    assert(end >= start);
    assert((end - start) <= INT64_MAX);
    return (int64_t) (end - start);
  }

  uint64_t posOffset() {
    assert(ordInBlock >= 0);
    decodePostings();
    assert(FieldType::hasPositions(flags));
    return posOffsets[(size_t) ordInBlock];
  }

  uint32_t pulsedValueIndex() {
    assert(isPulsed());
    uint32_t pulsedOrd = std::popcount(pulsedMask & ((1u << (uint32_t) ordInBlock) - 1));
    return pulsedOrd * (FieldType::hasPositions(flags) ? 2u : 1u);
  }

  int32_t pulsedDoc() {
    decodePostings();
    return (int32_t) pulsedValues[pulsedValueIndex()];
  }

  int32_t pulsedPos() {
    decodePostings();
    assert(FieldType::hasPositions(flags));
    return (int32_t) pulsedValues[pulsedValueIndex() + 1];
  }

  InputStream streamThrough(field_parts::Slice slice, ReadClass readClass = ReadClass::DATA) const {
    auto bytes = segmentCore.slice(slice, readClass);
    return {bytes.data(), bytes.data() + bytes.size()};
  }

  // Kept out of line so switching runs adds nothing to the block decode path.
  [[gnu::noinline]] void reselectRunForBlock(int32_t blockOrd) {
    auto key = blockOrd + 1;
    auto start = [](const auto& candidate) { return candidate.firstBlockOrd; };
    selectRun(findRun(key, start));
  }

  template <class Key, class Start>
  const field_parts::TermRun* findRun(Key key, Start start) const {
    if (key <= start(*run)) {
      // Random backward seek.
      return std::lower_bound(runs, run, key,
          [&](const auto& candidate, Key ordinal) { return start(candidate) < ordinal; }) - 1;
    }
    return runs + screaming::gallopLowerBound(
        run - runs + 1, runCount, key, [&](int64_t i) { return start(runs[i]); }) - 1;
  }

  void selectRun(const field_parts::TermRun* selected) {
    run = selected;
    bool hasNext = run + 1 != runs + runCount;
    runLastTermOrd = hasNext ? (run + 1)->firstTermOrd - 1 : totals.numTerms - 1;
    runEndBlockOrd = hasNext ? (run + 1)->firstBlockOrd : numTermBlocks;
    runTermsBase = (int64_t)run->dictionaryStart;
    runTermsEnd = (int64_t)(hasNext ? (run + 1)->dictionaryStart : termData->dictionaryBytes);
    runDocs = run->docs ? streamThrough(run->docs) : InputStream();
    runPositions = run->positions ? streamThrough(run->positions) : InputStream();
  }

  PackedTerm firstTermForNextBlock(int32_t nextBlock) const {
    auto base = nextBlock < runEndBlockOrd ? runTermsBase : runTermsEnd;
    return termsIS.readPackedTerm(base + termBlockOffsets[nextBlock]);
  }

  // Seeks to termBlockIndex and reads the eager block-entry state from the
  // format written by TextWriter::flushTerms: header values, hash pointer,
  // prefix/suffix length run pointers, suffix blob pointer, and metadata start.
  // It does not parse metadata run byte lengths or decode any stats/postings
  // metadata.  Those happen on demand in decodeStats and decodePostings.
  void readTermBlock() {
    // Common case (and always, for a single run): the block stays on the
    // current run; only re-resolve on a run boundary.  The whole dictionary
    // (the runs, block offsets and trie) lives in one
    // stream, so termsIS never needs reopening - only seeking.
    if (termBlockIndex < run->firstBlockOrd
        || termBlockIndex >= runEndBlockOrd) [[unlikely]] {
      reselectRunForBlock(termBlockIndex);
    }
    termsIS.seek(runTermsBase + (int64_t) termBlockOffsets[termBlockIndex]);
    startingOrd = run->firstTermForBlock(termBlockIndex);
    ordInBlock = 0;
    maxOrdInBlock = (int32_t) std::min<int64_t>(Postings::TERMS_BLOCK_SIZE - 1,
                                                runLastTermOrd - startingOrd);
    int64_t blockEndOffset = termBlockIndex + 1 < runEndBlockOrd
        ? runTermsBase + (int64_t)termBlockOffsets[termBlockIndex + 1] : runTermsEnd;
    blockEnd = termsIS.ptr(blockEndOffset);

    // see TextWriter::flushTerms
    startingTerm = termsIS.readPackedTerm();
    locOfDocsForTermBlock = (int64_t)termsIS.readVlong();
    locOfPositionsForTermBlock = (int64_t)termsIS.readVlong();
    blockPrefixLen = (uint8_t) termsIS.readByte();
    memcpy(&pulsedMask, termsIS.ptr(), sizeof(pulsedMask));
    termsIS.skip(sizeof(pulsedMask));
    suffixBytesTotal = termsIS.readVint();

    copyIntoTerm((char*) currTerm.ptr(), (const char*) startingTerm.ptr(), startingTerm.memorySize());
    // The block's first term is not delta-coded against anything, so nothing is
    // known to be shared with whatever term preceded it.
    sharedLen = 0;

    // remember, then skip over the term hashes... one byte per hash.
    termHashes = termsIS.ptr();
    termsIS.skip(maxOrdInBlock+1);  // ords are 0 based, so add 1 for the number of them. maxOrdInBlock is also inclusive (not one past the end)

    prefixLens = termsIS.ptr();
    termsIS.skip(maxOrdInBlock);
    suffixLens = termsIS.ptr();
    termsIS.skip(maxOrdInBlock);
    suffixBlob = termsIS.ptr();
    suffixCursor = suffixBlob;
    termsIS.skip(suffixBytesTotal);
    metadataRuns = termsIS.ptr();
    assert(metadataRuns <= blockEnd);

#ifndef NDEBUG
    // The cursor walks the blob by suffixLens alone, so the writer's declared
    // blob size has to be exactly what those lengths add up to.
    uint32_t suffixBytes = 0;
    for (int32_t i = 0; i < maxOrdInBlock; i++) {
      suffixBytes += (uint8_t) suffixLens[i];
    }
    assert(suffixBytes == suffixBytesTotal);
#endif

    statsDecoded = false;
    postingsDecoded = false;
    metadataRunsParsed = false;
  }

private:
  // The scan loop steps repeatedly on rejection.
  LUXIR_INLINE bool nextTermInScan() {
    if (ordInBlock == maxOrdInBlock) [[unlikely]] {
      if (termBlockIndex + 1 >= numTermBlocks) {
        return false;
      }
      termBlockIndex++;
      readTermBlock();
      return true;
    }

    readNextTermInBlock();
    return true;
  }


public:
  /// Advances to the next term, or returns false and leaves the ordinal alone.
  bool nextTerm() { return nextTermInScan(); }

protected:
  // reads the next term in the block with no checking if one runs off the end of the block.
  LUXIR_INLINE void readNextTermInBlock() {
    assert(ordInBlock < maxOrdInBlock);
    ordInBlock++;
    uint32_t prefixLen = (uint8_t) prefixLens[ordInBlock - 1];
    uint32_t suffixLen = (uint8_t) suffixLens[ordInBlock - 1];
    const char* suffix = suffixCursor;
    assert(suffix >= suffixBlob && suffix + suffixLen <= suffixBlob + suffixBytesTotal);
    suffixCursor = suffix + suffixLen;

    const char* data = currTerm.data();
    uint32_t len = currTerm.size();
    assert(len == currTerm.size());
    // Suffixes are short: usually one fixed 16-byte move (copyIntoTerm).

    assert(blockPrefixLen <= len);
    [[maybe_unused]] uint32_t factoredLen = len - blockPrefixLen;
    assert(prefixLen <= factoredLen);
    assert(blockPrefixLen + prefixLen + suffixLen <= PackedTerm::MAX_LEN);
    // The shared prefix ends at a differing byte or the end of the preceding term.
    assert(prefixLen == factoredLen || data[blockPrefixLen + prefixLen] != *suffix);

    copyIntoTerm(const_cast<char*>(data + blockPrefixLen + prefixLen), suffix, suffixLen);
    currTerm.setSize(blockPrefixLen + prefixLen + suffixLen);
    // These bytes come from the preceding term and form the shared prefix.
    sharedLen = (int32_t) (blockPrefixLen + prefixLen);
  }

  // Find the block whose separator key is the greatest one that is <= target,
  // then seek to and load that block, leaving the enum at ord 0 of it.
  // Targets in a gap between two blocks route to the later block; such targets
  // cannot be existing terms, and seekCeil must be able to land on that block.
  // Shared by seek(), seekCeilForward(), and seekCeil().
  void seekBlock(std::string_view target, int32_t firstBlock) {
    TrieReader trie(trieBase, (uint64_t)termData->trieRootOffset, numTermBlocks);
    termBlockIndex = trie.floorBlock(target);
    assert(termBlockIndex >= firstBlock);
    readTermBlock();
  }

public:
  // A seek can land arbitrarily far from where the enum was, so every seek ends
  // by dropping the shared prefix back to 0 (see sharedPrefixLen).  The
  // nTerms == 0 early returns need no such reset: sharedLen can only become
  // nonzero by advancing within a block, which an empty field never does.
  bool seek(std::string_view target) {
    skipCount(SkipStats::termDictionarySeeks);
    if (!termData) return false;
    seekBlock(target, 0);
    bool found = seekInBlock(target);
    sharedLen = 0;
    return found;
  }

  /// Forward-only ceil for sorted iteration. Target must be >= the current
  /// term. Positions on the smallest term >= target and returns true, or
  /// returns false at dictionary exhaustion. Unlike seekCeil, this never
  /// restarts before the current block.
  bool seekCeilForward(std::string_view target) {
    if (!termData) return false;
    if (termBlockIndex < 0) {
      termBlockIndex = 0;
      readTermBlock();
    } else {
      assert(!(target < term()));
    }

    int32_t nextBlock = termBlockIndex + 1;
    bool beyondCurrentBlock = (nextBlock < numTermBlocks) &&
        !(target < firstTermForNextBlock(nextBlock));

    if (beyondCurrentBlock) {
      // Load the block that may contain target, positioning at ord 0.  We
      // then fall into the same linear scan as the in-block case.  We do not
      // use seekInBlock here: its hash skip stops at the first hash-colliding term
      // past the target on a miss, leaving the enum beyond the true insertion
      // point, which would break the next seekForward (the post-miss position
      // must be the smallest term >= target for forward iteration to work).
      seekBlock(target, nextBlock);
    }

    while (term() < target) {
      if (!nextTerm()) {
        sharedLen = 0;
        return false;
      }
    }
    sharedLen = 0;
    return true;
  }

  /// Forward-only exact seek. On a miss with a ceil, remains positioned on
  /// that ceil; on exhaustion, remains on the last term.
  bool seekForward(std::string_view target) {
    // Historical callers treat an out-of-order target as a plain miss. Keep
    // that exact-seek behavior while seekCeilForward retains its stronger
    // monotonic precondition and positioned result contract.
    if (termBlockIndex >= 0 && target < term()) return false;
    return seekCeilForward(target) && term() == target;
  }

  /// Positions on the smallest term that is >= target.
  /// May move backward, so it can be called from any current position.
  /// Returns true with term() and ord() valid if such a term exists, or false
  /// if target sorts after every term.  To detect an exact match, compare
  /// term() to target after a true return.
  bool seekCeil(std::string_view target) {
    if (!termData) return false;
    // Start from the block whose first term is the greatest one <= target.
    seekBlock(target, 0);
    // nextTerm() can cross block boundaries, so this also handles a target
    // between the last term of one block and the first term of the next.
    bool found;
    for (;;) {
      auto cmp = term() <=> target;
      if (cmp >= 0) { found = true; break; }        // first term >= target
      if (!nextTerm()) { found = false; break; }    // target is past the last term
    }
    sharedLen = 0;
    return found;
  }

protected:
  bool seekInBlock(std::string_view target) {
    // TODO: rather than hashing every call, have an option to pass it in?
    char hash = (char)XXH3_64bits(target.data(), target.size());
    int lastOrd = ordInBlock - 1; // check the current term we are on.
    for(;;) {
      int matchOrd;
      // find the first matching hash
      for(matchOrd = lastOrd+1; matchOrd <= maxOrdInBlock; matchOrd++) {
        if (termHashes[matchOrd] == hash) break;
      }
      if (matchOrd > maxOrdInBlock) {
        return false;  // we got lucky and no more hashes matched!
      }

      // Hash code matched for the matchOrd term.
      // If we had a different term block structure, it might be easier to skip.  For now,
      // just call nextTerm and we get to skip the compare.
      while (ordInBlock < matchOrd) {
        readNextTermInBlock();
        // NOTE: there is one case where we do more work here than we should by skipping comparisons.
        // if the term does not exist, we could return earlier if we hit a term larger.
      }

      // Since the hash code matched, do actual comparison.  A test for equality would be faster if it's a match, but
      // we also want to handle the case where we went too far.
      auto cmp = term() <=> target;
      if (cmp == 0) return true;
      if (cmp > 0) return false;
      // if the term we just saw was smaller, continue where we left off
      lastOrd = ordInBlock;
    }
  }

  // Positions on the first term of the block containing the ordinal, if it exists.
  bool seekBlockOf(int64_t targetOrd) {
    if (targetOrd < 0 || targetOrd >= totals.numTerms) return false;
    const auto* selected = run;
    if (targetOrd < run->firstTermOrd || targetOrd > runLastTermOrd) {
      selected = findRun(targetOrd + 1, [](const auto& run) { return run.firstTermOrd; });
      selectRun(selected);
    }
    termBlockIndex = selected->blockForTerm(targetOrd);
    readTermBlock();
    return true;
  }

public:
  struct DocFreqBlock {
    int64_t firstOrd;  // 0-based ord of docFreqs[0]
    std::span<const uint32_t> docFreqs;
  };

  // The docFreqs of the whole term block holding 0-based targetOrd. Stats are
  // bulk-decoded once per block and no term bytes are rebuilt, so a walk over
  // consecutive blocks in either direction costs one decode per block. The
  // enum stays on a term of that block (its first, after a block load), so
  // seekOrd and nextTerm work as usual afterwards. The span is valid until
  // the enum moves to another block.
  DocFreqBlock docFreqBlock(int64_t targetOrd) {
    if (targetOrd < startingOrd || targetOrd > startingOrd + maxOrdInBlock) {
      if (!seekBlockOf(targetOrd)) return {-1, {}};
    }
    decodeStats();
    return {startingOrd, std::span<const uint32_t>(
        docFreqs.data(), (size_t) maxOrdInBlock + 1)};
  }

  // Positions at an existing zero-based term ordinal.
  void seekOrd(int64_t targetOrd) {
    if (targetOrd < ord() || targetOrd > startingOrd + maxOrdInBlock) {
      // even if we were in the right block, we don't have the capability to go backwards or rewind
      if (!seekBlockOf(targetOrd)) return;
    }
    while (ord() < targetOrd) {
      // nextTerm();
      readNextTermInBlock();
    }
    sharedLen = 0;
    assert(ord() == targetOrd);
  }


  // TODO: a push interface that can more quickly/directly handle pulsed postings while allowing inlining?
  // That could also handle differences between block and doc
};

}
