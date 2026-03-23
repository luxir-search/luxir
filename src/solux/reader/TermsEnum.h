#pragma once

#include "FieldReader.h"
#include "Postings.h"

namespace solux {
class TermsEnum {
  friend class DocsEnum;

  InputStream termsIS;
  MemPool& pool;
  PostingsReader& postingsReader;

  const SegFieldInfo& fieldInfo;

  PackedTerm currTerm;
  int32_t ordInBlock = -1; // the term number local to the current block
  int32_t docsSize;
  int32_t pulsedDoc;
  int32_t pulsedPos;

  // block-level information

  PackedTerm startingTerm;
  int32_t termBlockIndex = -1; // what term block are we currently in
  int32_t startingOrd = 0;
  int32_t maxOrdInBlock = -1;
  int64_t locOfDocsForTermBlock;  // absolute location... field offset + block offset
  int64_t locOfPositionsForTermBlock;  // absolute location... field offset + block offset
  int64_t cumulativeDocsSize;
  const char* termHashes;

  // term index level
  const int64_t* termBlockOffsets;
  int32_t numTermBlocks;

public:
  // fieldInfo is not copied and should remain valid throughout the lifetime of this TermsEnum and any related classes such as DocsEnum
  TermsEnum(MemPool& pool, PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) : pool(pool), postingsReader(postingsReader), fieldInfo(fieldInfo) {
    unused(this->pool, this->postingsReader);
    numTermBlocks = ((fieldInfo.nTerms-1) / Postings::TERMS_BLOCK_SIZE) + 1;
    termsIS = postingsReader.getInputStreamSeek(fieldInfo.termBlockIndexLoc);
    termBlockOffsets = reinterpret_cast<const int64_t*>(termsIS.ptr());  // offsets from termsLoc
    currTerm = PackedTerm(pool.alloc(PackedTerm::getMemSize(PackedTerm::MAX_BYTES)));
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
  PackedTerm term() const {
    return currTerm;
  }

  // read the data that comes after each term
  void readTermMetadata() {
    // see PostingsWriter.flushTerms
    docsSize = termsIS.readVint();
    cumulativeDocsSize += docsSize;
    if (docsSize == 0) {
      pulsedDoc = termsIS.readVint();
      pulsedPos = termsIS.readVint();
    }
  }

  // seeks to termBlockIndex and reads the block metadata + first term
  void readTermBlock() {
    termsIS.seek(fieldInfo.termsLoc.offset() + termBlockOffsets[termBlockIndex]);
    startingOrd = termBlockIndex * Postings::TERMS_BLOCK_SIZE;  // we currently have fixed size blocks
    cumulativeDocsSize = 0;
    ordInBlock = 0;
    maxOrdInBlock = std::min(Postings::TERMS_BLOCK_SIZE - 1, fieldInfo.nTerms - startingOrd - 1);

    // see PostingsWriter.flushTerms
    startingTerm = termsIS.readPackedTerm();
    locOfDocsForTermBlock = fieldInfo.docsLoc.offset() + termsIS.readVlong();  // fieldOffset + blockOffset for docs
    locOfPositionsForTermBlock = fieldInfo.posLoc.offset() + termsIS.readVlong();

    memcpy(currTerm.ptr(), startingTerm.ptr(), startingTerm.memorySize());

    // remember, then skip over the term hashes... one byte per hash.
    termHashes = termsIS.ptr();
    termsIS.skip(maxOrdInBlock+1);  // ords are 0 based, so add 1 for the number of them. maxOrdInBlock is also inclusive (not one past the end)

    readTermMetadata();
  }

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


  // reads the next term in the block with no checking if one runs off the end of the block.
  void readNextTermInBlock() {
    assert(ordInBlock < maxOrdInBlock);
    ordInBlock++;
    // read next suffix
    // see PostingsWriter.flushTerms
    uint8_t code = termsIS.readByte();
    auto prefixLen = code >> 5;
    auto suffixLen = (code & 0x1f) + 1;
    if (prefixLen == 7) {
      prefixLen = termsIS.readByte();
    }
    if (suffixLen == 32) {
      suffixLen = termsIS.readByte();
    }

    auto [data, len] = currTerm.unpack();
    // TODO: things to try:
    // - an explicit loop vs memcpy
    // - an explict loop of 8 bytes at a time... requires making sure there are extra bytes at the end of termIS file.
    //   - try it as a do-while loop... easier branch prediction?

    // try and catch unoptimal prefix compression (we had a bug before)
    assert(uint32_t(prefixLen) == len || data[prefixLen] != *termsIS.ptr());

    termsIS.read(const_cast<char*>(data + prefixLen), suffixLen);
    currTerm.setSize(prefixLen + suffixLen);

    readTermMetadata();
  }

  bool seek(std::string_view target) {
    auto blockEnd = termBlockOffsets + numTermBlocks;

    auto blockOffsetPtr = std::upper_bound(termBlockOffsets, blockEnd, target,
                                 [&](std::string_view key, const int64_t& blockOffset) {
      auto termAtBlock = termsIS.readPackedTerm(fieldInfo.termsLoc.offset() + blockOffset);
      return key < termAtBlock;
    });

    if (blockOffsetPtr > termBlockOffsets) {
      blockOffsetPtr--;
    }

    termBlockIndex = (int32_t)(blockOffsetPtr - termBlockOffsets);
    readTermBlock();
    return seekInBlock(target);
  }

  /// Forward-only seek for sorted iteration.
  /// TODO: optimize to narrow binary search range and avoid reloading same block.
  bool seekForward(std::string_view target) {
    return seek(target);
  }

  bool seekInBlock(std::string_view target) {
    // TODO: rather than hashing every segment, have an option to pass it in?
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