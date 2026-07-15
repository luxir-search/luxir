#pragma once

#include "solux/codec/StreamVByte.h"
#include "FieldReader.h"
#include "Postings.h"
#include "TrieReader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace solux {
class TermsEnum {
  friend class DocsEnum;

  InputStream termsIS;
  MemPool& pool;
  PostingsReader& postingsReader;

  const SegFieldInfo& fieldInfo;

  PackedTerm currTerm;
  int32_t ordInBlock = -1; // the term number local to the current block

  // block-level information

  PackedTerm startingTerm;
  int32_t termBlockIndex = -1; // what term block are we currently in
  int32_t startingOrd = 0;
  int32_t maxOrdInBlock = -1;
  int64_t locOfDocsForTermBlock;  // absolute location... field offset + block offset
  int64_t locOfPositionsForTermBlock;  // absolute location... field offset + block offset
  const char* termHashes;
  uint8_t blockPrefixLen = 0;
  uint32_t pulsedMask = 0;

  const char* prefixLens = nullptr;
  const char* suffixLens = nullptr;
  const char* suffixBlob = nullptr;
  const char* metadataRuns = nullptr;
  const char* blockEnd = nullptr;
  uint32_t suffixBytesTotal = 0;
  std::array<uint32_t, Postings::TERMS_BLOCK_SIZE> suffixStarts{};

  const char* docsEndRun = nullptr;
  const char* dfRun = nullptr;
  const char* ttfCodeRun = nullptr;
  const char* posOffRun = nullptr;
  const char* termImpactRun = nullptr;
  const char* pulsedRun = nullptr;
  uint32_t docsEndRunLen = 0;
  uint32_t dfRunLen = 0;
  uint32_t ttfCodeRunLen = 0;
  uint32_t posOffRunLen = 0;
  uint32_t termImpactRunLen = 0;
  uint32_t pulsedRunLen = 0;

  std::array<uint64_t, Postings::TERMS_BLOCK_SIZE> docsEnds{};
  std::array<uint32_t, Postings::TERMS_BLOCK_SIZE> docFreqs{};
  std::array<uint64_t, Postings::TERMS_BLOCK_SIZE> ttfCodes{};
  std::array<uint64_t, Postings::TERMS_BLOCK_SIZE> posOffsets{};
  std::array<uint32_t, Postings::TERMS_BLOCK_SIZE * 2> pulsedValues{};
  bool statsDecoded = false;
  bool postingsDecoded = false;
  bool suffixStartsDecoded = false;
  bool metadataRunsParsed = false;

  // term index level
  const int64_t* termBlockOffsets;
  InputStream trieIS;
  const char* trieBase;
  int32_t numTermBlocks;

public:
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
    int64_t docsStart = 0;
    int64_t docsEnd = 0;
    int64_t posStart = 0;
    int64_t totalTermFreq = 0;
    int32_t docFreq = 0;
    int32_t termOrdinal = -1;
    int32_t pulsedDoc = -1;
    int32_t pulsedPos = -1;
    bool hasFreqs = false;
    bool hasPositions = false;
  };

  // fieldInfo is not copied and should remain valid throughout the lifetime of this TermsEnum and any related classes such as DocsEnum
  TermsEnum(MemPool& pool, PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) : pool(pool), postingsReader(postingsReader), fieldInfo(fieldInfo) {
    unused(this->pool, this->postingsReader);
    numTermBlocks = ((fieldInfo.nTerms-1) / Postings::TERMS_BLOCK_SIZE) + 1;
    termsIS = postingsReader.getInputStreamSeek(fieldInfo.termBlockIndexLoc);
    termBlockOffsets = reinterpret_cast<const int64_t*>(termsIS.ptr());  // offsets from termsLoc
    trieIS = postingsReader.getInputStreamSeek(fieldInfo.trieLoc);
    trieBase = trieIS.ptr();
    currTerm = PackedTerm(pool.alloc(PackedTerm::getMemSize(PackedTerm::MAX_BYTES)), 0);
  }

  int32_t numTerms() const {
    return fieldInfo.nTerms;
  }

  int32_t docsWithField() const {
    return fieldInfo.docsWithField;
  }

  /// sumDocFreq is a field-level stat:
  /// The docFreq of a term is the number of documents it appears in. sumDocFreq is the sum across all terms in this field.
  /// If sumDocFreq() == docsWithField() then every document contains only one term (or the same term repeated.)  May be
  /// useful for detecting single-valued fields even if they were not marked as such.
  int64_t sumDocFreq() const {
    return fieldInfo.sumDocFreq;
  }

  /// sumTotalTermFreq is a field-level stat:
  /// termFreq is the number of times the term appears in a single document.
  /// totalTermFreq is the number of times the term appears across all documents in this segment.
  /// sumTotalTermFreq is the sum of totalTermFreq for all terms in this field (i.e. number of tokens indexed)
  int64_t sumTotalTermFreq() const {
    return fieldInfo.sumTotalTermFreq;
  }

  /// returns the 0-based ordinal of the current term.
  int32_t ord() const {
    return startingOrd + ordInBlock;
  }

  /// NOTE: the returned term is invalidated/changed if this TermsEnum is moved off this
  /// term (i.e. the moment next() or seek() is called). Make a copy if you wish to keep it!
  /// If called before nextTerm() or seek() is done, returns a 0 length term.
  PackedTerm term() const {
    return currTerm;
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
    if (FieldType::hasFreqs(fieldInfo.flags)) {
      ttf += ttfCodes[(size_t) ordInBlock];
    }
    assert(ttf <= INT64_MAX);
    return (int64_t) ttf;
  }

  // The whole-term (norm, maxTf) Pareto frontier stored in the term block
  // metadata (see PostingsWriter.flushTerms): the source for a scorer's
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
    return FieldType::hasFreqs(fieldInfo.flags) && FieldType::hasPositions(fieldInfo.flags);
  }

  EncodedImpactFrontier currentTermImpactFrontierSpan() {
    assert(ordInBlock >= 0);
    if (!hasTermImpacts()) {
      return {};
    }
    parseMetadataRuns();
    const char* p = termImpactRun;
    const char* end = termImpactRun + termImpactRunLen;
    for (int32_t t = 0; t < ordInBlock; t++) {
      skipEncodedImpactFrontier(p, end);
    }
    const char* start = p;
    skipEncodedImpactFrontier(p, end);
    assert(p <= end);
    return {start, (uint32_t) (p - start)};
  }

  PostingsState postingsState() {
    assert(ordInBlock >= 0);
    decodePostings();

    PostingsState state;
    state.hasFreqs = FieldType::hasFreqs(fieldInfo.flags);
    state.hasPositions = FieldType::hasPositions(fieldInfo.flags);
    state.termOrdinal = ord();
    state.docFreq = docFreq();
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
      state.docIS = postingsReader.getInputStream(fieldInfo.docsLoc.filenum());
      state.docIS.seek(state.docsStart);
      if (state.hasPositions) {
        state.posStart = locOfPositionsForTermBlock + (int64_t) posOffsets[(size_t) ordInBlock];
        state.posIS = postingsReader.getInputStream(fieldInfo.posLoc.filenum());
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

  void decodeSuffixStarts() {
    if (suffixStartsDecoded) {
      return;
    }
    uint32_t suffixBytes = 0;
    for (int32_t i = 0; i < maxOrdInBlock; i++) {
      suffixStarts[(size_t) i] = suffixBytes;
      suffixBytes += (uint8_t) suffixLens[i];
    }
    assert(suffixBytes == suffixBytesTotal);
    suffixStartsDecoded = true;
  }

  void parseMetadataRuns() {
    if (metadataRunsParsed) {
      return;
    }
    // Metadata run lengths are parsed only when a stats or postings accessor
    // asks for them.  This keeps block entry and pure term scans on the
    // hashes/lengths/suffix path.  See PostingsWriter.flushTerms for the
    // byte-length code and run order.  A zero byte length marks an all-default
    // run for docsEnd/df/ttfCode/posOff; pulsedRun still uses zero length only
    // for the existing empty-payload case.
    const char* p = metadataRuns;
    docsEndRunLen = readMetadataRunLen(p, blockEnd);
    dfRunLen = readMetadataRunLen(p, blockEnd);
    ttfCodeRunLen = FieldType::hasFreqs(fieldInfo.flags) ? readMetadataRunLen(p, blockEnd) : 0;
    posOffRunLen = FieldType::hasPositions(fieldInfo.flags) ? readMetadataRunLen(p, blockEnd) : 0;
    termImpactRunLen = hasTermImpacts() ? readMetadataRunLen(p, blockEnd) : 0;
    pulsedRunLen = readMetadataRunLen(p, blockEnd);

    docsEndRun = p;
    p += docsEndRunLen;
    assert(p <= blockEnd);
    dfRun = p;
    p += dfRunLen;
    assert(p <= blockEnd);
    if (FieldType::hasFreqs(fieldInfo.flags)) {
      ttfCodeRun = p;
      p += ttfCodeRunLen;
      assert(p <= blockEnd);
    } else {
      ttfCodeRun = nullptr;
    }
    if (FieldType::hasPositions(fieldInfo.flags)) {
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
    uint8_t* dataEnd = svb_decode_avx_simple(out, (uint8_t*) run, (uint8_t*) run + keyBytes, count);
    assert(dataEnd == (uint8_t*) run + runLen);
  }

  void decodeStats() {
    if (statsDecoded) {
      return;
    }
    // Tier 1 metadata: term statistics only.  This decodes df and, for fields
    // with freqs, ttfCode.  It deliberately does not touch docsEnd, posOff, or
    // pulsed values, so stats-only callers do not pay to open postings.
    parseMetadataRuns();
    uint32_t n = blockTermCount();
    if (dfRunLen == 0) {
      std::fill_n(docFreqs.begin(), n, 1);
    } else {
      decodeSVBRun(dfRun, dfRunLen, docFreqs.data(), n);
    }
    if (FieldType::hasFreqs(fieldInfo.flags)) {
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
    // trailer-free docs slices, posOff supplies positions starts, and pulsedRun
    // supplies inline doc/pos payloads for single-occurrence terms.
    parseMetadataRuns();
    uint32_t n = blockTermCount();
    if (docsEndRunLen == 0) {
      assert((pulsedMask & validPulsedMask()) == validPulsedMask());
      std::fill_n(docsEnds.begin(), n, 0);
    } else {
      const char* p = docsEndRun;
      const char* end = docsEndRun + docsEndRunLen;
      uint64_t prevDocsEnd = 0;
      for (uint32_t i = 0; i < n; i++) {
        uint64_t docsEnd = InputStream::readVlong(p, end);
        assert(docsEnd >= prevDocsEnd);
        docsEnds[i] = docsEnd;
        prevDocsEnd = docsEnd;
      }
      assert(p == end);
    }

    if (FieldType::hasPositions(fieldInfo.flags)) {
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
    uint32_t pulsedCount = pulsedTerms * (FieldType::hasPositions(fieldInfo.flags) ? 2u : 1u);
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
    assert(FieldType::hasPositions(fieldInfo.flags));
    return posOffsets[(size_t) ordInBlock];
  }

  uint32_t pulsedValueIndex() {
    assert(isPulsed());
    uint32_t pulsedOrd = std::popcount(pulsedMask & ((1u << (uint32_t) ordInBlock) - 1));
    return pulsedOrd * (FieldType::hasPositions(fieldInfo.flags) ? 2u : 1u);
  }

  int32_t pulsedDoc() {
    decodePostings();
    return (int32_t) pulsedValues[pulsedValueIndex()];
  }

  int32_t pulsedPos() {
    decodePostings();
    assert(FieldType::hasPositions(fieldInfo.flags));
    return (int32_t) pulsedValues[pulsedValueIndex() + 1];
  }

  // Seeks to termBlockIndex and reads the eager block-entry state from the
  // format written by PostingsWriter.flushTerms: header values, hash pointer,
  // prefix/suffix length run pointers, suffix blob pointer, and metadata start.
  // It does not prefix-sum suffixLen, parse metadata run byte lengths, or decode
  // any stats/postings metadata.  Those happen on demand in decodeSuffixStarts,
  // decodeStats, and decodePostings.
  void readTermBlock() {
    termsIS.seek(fieldInfo.termsLoc.offset() + termBlockOffsets[termBlockIndex]);
    startingOrd = termBlockIndex * Postings::TERMS_BLOCK_SIZE;  // we currently have fixed size blocks
    ordInBlock = 0;
    maxOrdInBlock = std::min(Postings::TERMS_BLOCK_SIZE - 1, fieldInfo.nTerms - startingOrd - 1);
    int64_t blockEndOffset = termBlockIndex + 1 < numTermBlocks
        ? fieldInfo.termsLoc.offset() + termBlockOffsets[termBlockIndex + 1]
        : fieldInfo.termBlockIndexLoc.offset();
    blockEnd = termsIS.ptr(blockEndOffset);

    // see PostingsWriter.flushTerms
    startingTerm = termsIS.readPackedTerm();
    locOfDocsForTermBlock = fieldInfo.docsLoc.offset() + termsIS.readVlong();  // fieldOffset + blockOffset for docs
    locOfPositionsForTermBlock = fieldInfo.posLoc.offset() + termsIS.readVlong();
    blockPrefixLen = (uint8_t) termsIS.readByte();
    memcpy(&pulsedMask, termsIS.ptr(), sizeof(pulsedMask));
    termsIS.skip(sizeof(pulsedMask));
    suffixBytesTotal = termsIS.readVint();

    memcpy(currTerm.ptr(), startingTerm.ptr(), startingTerm.memorySize());

    // remember, then skip over the term hashes... one byte per hash.
    termHashes = termsIS.ptr();
    termsIS.skip(maxOrdInBlock+1);  // ords are 0 based, so add 1 for the number of them. maxOrdInBlock is also inclusive (not one past the end)

    prefixLens = termsIS.ptr();
    termsIS.skip(maxOrdInBlock);
    suffixLens = termsIS.ptr();
    termsIS.skip(maxOrdInBlock);
    suffixBlob = termsIS.ptr();
    termsIS.skip(suffixBytesTotal);
    metadataRuns = termsIS.ptr();
    assert(metadataRuns <= blockEnd);

    statsDecoded = false;
    postingsDecoded = false;
    suffixStartsDecoded = false;
    metadataRunsParsed = false;
  }

public:
  /// If there is a next term, this advances to it and returns true.
  /// Otherwise, no advance is done (i.e. ord() is not changed.)
  bool nextTerm() {
    if (ordInBlock == maxOrdInBlock) {
      if (ord() + 1 >= fieldInfo.nTerms) {  // could also compare number of blocks to detect end.
        return false;
      }
      termBlockIndex++;
      readTermBlock();
      return true;
    }

    readNextTermInBlock();
    return true;
  }


protected:
  // reads the next term in the block with no checking if one runs off the end of the block.
  void readNextTermInBlock() {
    assert(ordInBlock < maxOrdInBlock);
    decodeSuffixStarts();
    ordInBlock++;
    uint32_t prefixLen = (uint8_t) prefixLens[ordInBlock - 1];
    uint32_t suffixLen = (uint8_t) suffixLens[ordInBlock - 1];
    const char* suffix = suffixBlob + suffixStarts[(size_t) ordInBlock - 1];

    auto [data, len] = currTerm.unpack();
    // TODO: things to try:
    // - an explicit loop vs memcpy
    // - an explict loop of 8 bytes at a time... requires making sure there are extra bytes at the end of termIS file.
    //   - try it as a do-while loop... easier branch prediction?

    assert(blockPrefixLen <= len);
    uint32_t factoredLen = len - blockPrefixLen;
    assert(prefixLen <= factoredLen);
    assert(blockPrefixLen + prefixLen + suffixLen <= PackedTerm::MAX_LEN);
    // prefixLen is measured against the previous term after factoring out
    // blockPrefixLen. The old "cannot extend by one more byte" assertion now
    // compares at that factored boundary.
    assert(prefixLen == factoredLen || data[blockPrefixLen + prefixLen] != *suffix);

    memcpy(const_cast<char*>(data + blockPrefixLen + prefixLen), suffix, suffixLen);
    currTerm.setSize(blockPrefixLen + prefixLen + suffixLen);
  }

  // Find the block whose separator key is the greatest one that is <= target,
  // then seek to and load that block, leaving the enum at ord 0 of it.
  // Targets in a gap between two blocks route to the later block; such targets
  // cannot be existing terms, and seekCeil must be able to land on that block.
  // Shared by seek(), seekForward(), and seekCeil().
  void seekBlock(std::string_view target, int32_t firstBlock) {
    TrieReader trie(trieBase, (uint64_t)fieldInfo.trieRootOff, numTermBlocks);
    termBlockIndex = trie.floorBlock(target);
    assert(termBlockIndex >= firstBlock);
    readTermBlock();
  }

public:
  bool seek(std::string_view target) {
    seekBlock(target, 0);
    return seekInBlock(target);
  }

  /// Forward-only seek for sorted iteration. Target must be >= the current term.
  /// If the target is in the current block, scans forward with nextTerm().
  /// Otherwise descends the trie and asserts the result is at or after the next block.
  /// Can be called without a prior seek() - the first call will load the first block.
  bool seekForward(std::string_view target) {
    if (termBlockIndex < 0) {
      // Not yet positioned - load first block
      termBlockIndex = 0;
      readTermBlock();
    }

    int32_t nextBlock = termBlockIndex + 1;
    bool beyondCurrentBlock = (nextBlock < numTermBlocks) &&
        !(target < termsIS.readPackedTerm(fieldInfo.termsLoc.offset() + termBlockOffsets[nextBlock]));

    if (beyondCurrentBlock) {
      // Load the block that may contain target, positioning at ord 0.  We
      // then fall into the same linear scan as the in-block case.  We do not
      // use seekInBlock here: its hash skip stops at the first hash-colliding term
      // past the target on a miss, leaving the enum beyond the true insertion
      // point, which would break the next seekForward (the post-miss position
      // must be the smallest term >= target for forward iteration to work).
      seekBlock(target, nextBlock);
    }

    // Linear forward scan within the current block.  Stops at the first term
    // >= target (the insertion point), so a subsequent seekForward starts from
    // the correct position.
    for (;;) {
      auto cmp = term() <=> target;
      if (cmp == 0) return true;
      if (cmp > 0) return false;
      if (ordInBlock >= maxOrdInBlock) return false;
      readNextTermInBlock();
    }
  }

  /// Positions on the smallest term that is >= target.
  /// May move backward, so it can be called from any current position.
  /// Returns true with term() and ord() valid if such a term exists, or false
  /// if target sorts after every term.  To detect an exact match, compare
  /// term() to target after a true return.
  bool seekCeil(std::string_view target) {
    // Start from the block whose first term is the greatest one <= target.
    seekBlock(target, 0);
    // nextTerm() can cross block boundaries, so this also handles a target
    // between the last term of one block and the first term of the next.
    for (;;) {
      auto cmp = term() <=> target;
      if (cmp >= 0) return true;        // first term >= target
      if (!nextTerm()) return false;    // target is past the last term
    }
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

public:
  // 0-based ords
  void seekOrd(int32_t targetOrd) {
    assert(targetOrd >= 0 && targetOrd < fieldInfo.nTerms);
    if (targetOrd < ord() || targetOrd > startingOrd + maxOrdInBlock) {
      // even if we were in the right block, we don't have the capability to go backwards or rewind
      termBlockIndex = uint32_t(targetOrd) / Postings::TERMS_BLOCK_SIZE;
      readTermBlock();
    }
    while (ord() < targetOrd) {
      // nextTerm();
      readNextTermInBlock();
    }
    assert(ord() == targetOrd);
  }


protected:
  bool seekCeilInBlock(std::string_view target) {
    auto cmp = term() <=> target;
    // std::cout << " comparing with first " << term() << ": eq=" << (cmp==0) << " gt=" <<  (cmp>0) << std::endl;
    if (cmp == 0) return true;
    if (cmp > 0) return false;  // this will normally only happen on the *first* block

    // TODO: we could optimize this seeking by not actually building the term to compare.
    // We know from prefix encoding how much of the previous term is shared.
    // It could also possibly be faster to skip term metadata rather than reading it as well.
    while (ordInBlock < maxOrdInBlock) {
      nextTerm();
      cmp = term() <=> target;
      // std::cout << " comparing with next " << term() << ": eq=" << (cmp==0) << " gt=" <<  (cmp>0) << std::endl;
      if (cmp == 0) return true;
      if (cmp > 0) return false;
    }
    return false;
  }


  // TODO: a push interface that can more quickly/directly handle pulsed postings while allowing inlining?
  // That could also handle differences between block and doc
};

}
