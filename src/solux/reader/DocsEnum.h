#pragma once

#include "TermsEnum.h"
#include "solux/codec/Codec.h"

namespace solux {
// TODO: templatize to be able to instrument, implement checkindex, etc...
// TODO: investigate writing a version of this based on continuations and see how it performs?
// TODO: some of this internal state could be removed... we only need some of it in the constructor?
// Implementation note: moving block reading of docs and positions to cpp files and just leaving the hot path
// in the header resulted in >3% performance loss for docs, and >5% loss for positions.
class DocsEnum {
  // Position ordinals are indexes into the term-global positions list for non-pulsed positions.
  // Thus the max posOrd should thus be totalTermFreq (except in the case of a single pulsed term,
  // in which case there is nothing to read from the posFile anyway).
  int32_t* posBuf;       // the list of decoded position deltas (may be partial)
  int32_t* docBuf;       // the list of decoded docs (may be partial)
  int32_t* tfreqBuf;     // the list of decoded term freqs

  int64_t posOrd = 0;         // the ordinal of the current position we are on
  int64_t posOrdStart = 0;    // the starting position ordinal for the current doc
  int64_t cumulativeTermFreq; // Synonym for posOrdEnd.  Should be equal to posOrdStart + tfreq (i.e. a docs positions are [posOrdStart,cumulativeTermFreq)

  int32_t posBufIdx = 0; // index of the next value to read in the position buffer
  int32_t posBufEnd;     // one-past the last decoded position delta (but may be past the deltas for *this* doc
  int32_t posBufEndDoc;  // one-past the last decoded pos delta for *this* doc
  int32_t pos;           // current position

  int32_t docOrd = 0;    // the ordinal of the current document we are on for this term
  int32_t docBufIdx = 0; // index of the next value to read in the docs buffer
  int32_t docBufEnd;     // index of one-past the last valid element
  int32_t docid;         // current docid

  // For term freqs, since they are parallel to docs, we don't actually need
  // all of these variables.  But it sets the stage for separating the two
  // more and using different encodings / block sizes for them.
  int32_t tfreqOrd = 0;      // ordinal of the current term freq (parallel to docOrd)
  int32_t tfreqBufIdx = 0;   // index of the next valid value
  int32_t tfreqBufEnd;       // index of one-past the last valid element
  int32_t tfreq;


  InputStream docIS;
  InputStream posIS;
  PostingsReader& postingsReader;
  const SegFieldInfo& fieldInfo;
  MemPool* pool;
  int32_t docfreq; // number of docs containing this term
  int64_t ttf;    // totalTermFreq (sum of term freq across all docs for this term)

  int32_t docsSize;  // size of the postings in the doc file for the given term
  int64_t startOfDocs;

  int64_t cumulativeDocsSize;
  int64_t locOfDocsForTermBlock;
  int64_t locOfPositionsForTermBlock;


  int32_t db[Postings::DOCS_BLOCK_SIZE];  // temporary...
  int32_t pb[Postings::POSITIONS_BLOCK_SIZE];
  int32_t tb[Postings::POSITIONS_BLOCK_SIZE];

  // returns the ord of the last position in the last full block.. i.e. for 150 positions, it would return 127 (0-127 are in first block)
  // for 10, it would return -1 (there are no block encoded positions)
  static int64_t lastBlockEncodedPosOrd(uint64_t ttf) {
    // A ttf of 127 means we don't have a full block... so return -1.
    // A ttf of 128 through (128+127) means ords 0-127 are in first block and we would want to return 127.
    return (ttf & ~(Postings::POSITIONS_BLOCK_SIZE-1)) - 1;
  }

public:
  /// sentinel value used for both docs and positions
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  // After this constructor has finished, this DocsEnum instance is independent of the TermsEnum instance.
  // This instance *does* rely on fieldInfo that was passed into the TermsEnum instance still being valid.
  DocsEnum(MemPool& pool, PostingsReader& postingsReader, TermsEnum& tenum,
           int32_t* docsScratch=nullptr, int32_t* posScratch=nullptr, int32_t* tfreqScratch=nullptr)
  : postingsReader(postingsReader), fieldInfo(tenum.fieldInfo), pool(&pool)
  {
    unused(docsScratch, posScratch, tfreqScratch);
    docBuf=db;
    posBuf=pb;
    tfreqBuf=tb;

    // Since the same terms enum will often be used for multiple docs enum, we should copy everything we need
    // from the terms enum that we need (that may change.)
    // TODO: package those dependencies in a struct that can be simply assigned?  Or if there is enough overlap, simply copy the complete tenum?
    // Or we could invert the responsibility and make the client copy the tenum if they are going to change it.
    docsSize = tenum.docsSize;
    docid = -1;

    // TODO: look into deferring filling the buffer until we need it, then we can avoid allocating the buffers for a shared DocsEnum.
    if (docsSize == 0) {
      // postings pulsed
      docfreq = 1;
      tfreq = 1;
      ttf = 1;
      // fill buffers with single pulsed doc+position
      docBuf[0] = tenum.pulsedDoc;
      docBufEnd = 1;
      tfreqBuf[0] = 1;
      tfreqBufEnd = 1;
      posBuf[0] = tenum.pulsedPos;
      posBufIdx = 0;
      posBufEndDoc = posBufEnd = 1;
      cumulativeTermFreq = 0;  // this will be incremented in nextDoc()
      // std::cout << "Pulsed posting: id=" << docid << "pos=" << posBuf[0] << std::endl;

    } else {
      pos = tfreq = -1;  // unnecessary initializations, but it makes some maybe-uninitialized warnings go away with -O3  // todo: revisit
      locOfDocsForTermBlock = tenum.locOfDocsForTermBlock;
      locOfPositionsForTermBlock = tenum.locOfPositionsForTermBlock;
      cumulativeDocsSize = tenum.cumulativeDocsSize;
      docIS = postingsReader.getInputStream(fieldInfo.docsLoc.filenum());

      // see the end of PostingsWriter.endTerm() for the term-specific metadata written there (docfreq, ttf, etc)

      // read last byte of docs to get the metadata size
      docIS.seek(locOfDocsForTermBlock + cumulativeDocsSize - 1);
      uint8_t metaSize = docIS.readByte();
      docIS.relativeSeek(-metaSize - 1); // move to start of metadata
      docfreq = docIS.readVint();
      ttf = docfreq + docIS.readVlong();
      docBufEnd = 0;

      auto posOffset = docIS.readVlong();

      // start of the actual docs is end of block - size
      startOfDocs = locOfDocsForTermBlock + cumulativeDocsSize - docsSize;
      docIS.seek(startOfDocs);

      posIS = postingsReader.getInputStream(fieldInfo.posLoc.filenum());
      posIS.seek(locOfPositionsForTermBlock + posOffset);
      posBufEndDoc = posBufEnd = 0; // no positions read yet
      cumulativeTermFreq = 0;
      // std::cout << "Normal posting" << std::endl;
    }
  }

  DocsEnum(const DocsEnum& other) = delete;

  DocsEnum(MemPool& pool, const DocsEnum& other) : postingsReader(other.postingsReader), fieldInfo(other.fieldInfo) {
    memcpy(this, &other, sizeof(DocsEnum));  // is there a better way to copy everything that can be copied so we don't forget anything?
    // re-point the internal pointers
    this->pool = &pool; // new pool (currently unused though since buffers are immediate)
    docBuf = db;
    posBuf = pb;
    tfreqBuf = tb;
    // If buffers cease to be immediate, we need to copy in the pulsed docs/positions/freqs (first element)
  }

  /// number of documents containing the term
  int32_t numDocs() {
    return docfreq;
  }

  /// sum of term freq across all documents (i.e. total number of appearances for this term)
  int32_t totalTermFreq() {
    return ttf;
  }

  /// the document this iterator is currently positions on
  int32_t docId() {
    return docid;
  }

  /// number of times the term appears in the current document
  int32_t termFreq() {
    return tfreq;
  }

  int32_t nextDocOld() {
    if (docsSize != 0) {
      // see PostingsWriter.endTerm() for format of non-block encoded docs/freqs
      uint32_t doccode = docIS.readVint();
      if ((doccode & 0x01)==1) {
        tfreq = 1;
      } else {
        tfreq = docIS.readVint();
      }
      posOrdStart = cumulativeTermFreq;
      cumulativeTermFreq += tfreq;
      auto docDelta = doccode >> 1;
      docid += docDelta;
    }
    return docid;
  }

  // we also have a next() to align with scorers
  int32_t next() {
    return nextDoc();
  }

  int32_t nextDoc() {
    // Contract: callers must not re-poll after END (see Query::Scorer).
    assert(docid != PostingsReader::END);
    if (docBufIdx >= docBufEnd) {
      auto leftToRead = docfreq - docOrd;
      // Boundary analysis: if docfreq==1 and docOrd==1 (meaning we already read ord 0, but not 1), we are done.
      if (leftToRead <= 0) {
        assert(leftToRead == 0);
        docid = PostingsReader::END;
        return docid;
      }

      // Since we only read whole blocks, simply comparing with number of docs left to read is sufficient.
      // If we start partial decoding of blocks (say because of skipping), then we would want something
      // like lastBlockEncodedPosOrd, but for docs.
      if (leftToRead >= Postings::DOCS_BLOCK_SIZE) {
        uint32_t outSz = Postings::DOCS_BLOCK_SIZE;
        auto bytesRead = IndexCodec::docCodec.decodeBlock(docIS.ptr(), docIS.left(), (uint32_t*)docBuf, outSz);
        docIS.skip(bytesRead);
        assert(outSz == Postings::DOCS_BLOCK_SIZE);
        docBufIdx = 0;
        docBufEnd = Postings::DOCS_BLOCK_SIZE;
        // std::cout << "read doc block: " << std::endl;

        // TODO: we should really decode term freqs lazily in case they aren't needed... but this is far simpler for now.
        outSz = Postings::DOCS_BLOCK_SIZE;  // currently parallel to docs, so must be same block size
        bytesRead = IndexCodec::tfreqCodec.decodeBlock(docIS.ptr(), docIS.left(), (uint32_t*)tfreqBuf, outSz);
        docIS.skip(bytesRead);
        assert(outSz == Postings::DOCS_BLOCK_SIZE);
        tfreqBufIdx = 0;
        tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
        // std::cout << "read tfreq block: " << std::endl;
      } else {
        // decode whole tail?
        // int32_t id = docid;  // PostingsWriter currently uses -1 for tail base, not lastDoc
        int32_t id = -1;
        for (int i=0; i<leftToRead; i++) {
          // see PostingsWriter.endTerm() for format of non-block encoded docs/freqs
          uint32_t doccode = docIS.readVint();
          int32_t tf;
          if ((doccode & 0x01) == 1) {
            tf = 1;
          } else {
            tf = docIS.readVint();
          }
          auto docDelta = ((uint32_t)doccode) >> 1;
          id += docDelta;
          docBuf[i] = id;
          tfreqBuf[i] = tf;
        }

        docBufIdx = 0;
        docBufEnd = leftToRead;
        tfreqBufIdx = 0;
        tfreqBufEnd = leftToRead;
        // std::cout << "read doc/tfreq tail: " << leftToRead << std::endl;

      } // end decode tail
    }

    // NOTE: because tfreq and cumulativeTermFreq are currently directly read used when reading positions,
    // keep these up-to-date for now instead of lazily calculating.
    assert(docBufIdx == tfreqBufIdx);
    assert(docOrd == tfreqOrd);

    docid = docBuf[docBufIdx++];
    docOrd++;

    tfreq = tfreqBuf[tfreqBufIdx++];
    tfreqOrd++;
    posOrdStart = cumulativeTermFreq;
    cumulativeTermFreq += tfreq;

    return docid;
  }


  // Strict (like the Scorer / Lucene PostingsEnum contract): target must be
  // beyond the current doc. The only caller, PhraseQuery::doNext, guards.
  int32_t advance(int32_t target) {
    assert(docid < target);
    while (docid < target) {
      nextDoc();
    }
    return docid;
  }

  void startPositions() {
    pos = -1;
    while (posOrd < posOrdStart) {
      // need to skip some positions.
      auto numToSkip = posOrdStart - posOrd;
      auto leftInBlock = posBufEnd - posBufIdx;

      if (numToSkip <= leftInBlock) {
        // std::cout << "skipping within position block: id=" << docid << " numToSkip=" << numToSkip << std::endl;
        // our start is within current decoded block
        posBufIdx += numToSkip;
        posOrd += numToSkip;
        posBufEndDoc = std::min(posBufIdx + tfreq, posBufEnd);
        return;
      }

      // skip to end of block first.
      // some of these calculations may be redundant, but it's just to make it easier to think about for now.
      // Hopefully optimizer can take care of inefficiencies.
      posOrd += leftInBlock;
      numToSkip -= leftInBlock;
      posBufIdx = posBufEnd;

      if (numToSkip >= Postings::POSITIONS_BLOCK_SIZE) {
        // TODO: skip whole block
        // std::cout << "skipping blocks: id=" << docid << " numToSkip=" << numToSkip << std::endl;
        // For now, just decode the whole block.  Optimize this later.
        uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
        auto bytesRead = IndexCodec::posCodec.decodeBlock(posIS.ptr(), posIS.left(), (uint32_t*)posBuf, outSz);
        posIS.skip(bytesRead);
        assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
        posBufIdx = 0;
        posBufEnd = outSz;
        // posBufEndDoc = std::min(posBufEnd, tfreq);  // not needed, we will be skipping the block
        continue;
      }


      // At this point, we need to skip less than a block of positions, but we need to know
      // if it is block encoded or vInt encoded.  Compare to last block encoded ord to tell.
      // Boundary conditions: if posOrdStart=127 then it is the last in the block and we
      // do want to decode the block (hence do tail logic otherwise)
      if (posOrdStart > lastBlockEncodedPosOrd(ttf)) {
        // std::cout << "skipping in position tail: id=" << docid << " numToSkip=" << numToSkip << std::endl;
        for (int i=0; i<numToSkip; i++) {
          auto delta = posIS.readVint();
          unused(delta);
          // TODO: a faster skipVint (potentially)? inlining should already eliminate the dead code though.
        }
        posOrd += numToSkip;
        assert (posOrd == posOrdStart); // nocommit, trivial
        posBufIdx = posBufEnd = posBufEndDoc = 0;
        return;
      }

      // OK load block of positions.  This could be optimized by only loading the relevant part.
      // If this enum wants all positions, we should just decode everything.
      uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
      auto bytesRead = IndexCodec::posCodec.decodeBlock(posIS.ptr(), posIS.left(), (uint32_t*)posBuf, outSz);
      posIS.skip(bytesRead);
      assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
      posBufIdx = 0;
      posBufEnd = outSz;
      posBufEndDoc = std::min(posBufEnd, tfreq);
    }
  }

  int32_t nextPosition() {
    if (posBufIdx >= posBufEndDoc) {
      // Reached the end of buffered pos deltas, either because
      // there are no more positions for this doc, or because we need
      // to reload more.

      int leftToRead = (int)(cumulativeTermFreq - posOrd); // should never be larger than 32 bit int

      if (leftToRead <= 0) {
        assert(posOrd == cumulativeTermFreq); // should not have gone past
        // TODO: set pos to something, or keep last valid position?
        pos = PostingsReader::END;
        return pos;
      }

      // we moved to a new doc, but still have positions decoded in the buffer.
      // TODO: it feels like we should move this case to startPositions()
      if (posBufEnd > posBufEndDoc) {
        // just update our new end pointer
        posBufEndDoc = std::min(posBufEnd, posBufIdx+leftToRead);
      } else {
        // if we have more positions to read, then there should be no more buffered
        assert(posBufEndDoc == posBufEnd);

        if (posOrd <= lastBlockEncodedPosOrd(ttf)) {
          // read a new block of positions
          // TODO: refactor reading a new block (not skipping) to one place?
          uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
          auto bytesRead = IndexCodec::posCodec.decodeBlock(posIS.ptr(), posIS.left(), (uint32_t*)posBuf, outSz);
          posIS.skip(bytesRead);
          assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
          posBufIdx = 0;
          posBufEnd = outSz;
          posBufEndDoc = std::min(posBufEnd, tfreq);
        } else {
          // we are in the tail
          // we could read position-by-position at the tail if we wanted...
          // we could also handle pulsed positions here instead if sticking them in the buffer.
          posBufIdx = 0;
          posBufEnd = leftToRead; // only read enough for this doc
          posBufEndDoc = leftToRead;
          // std::cout << "FILL pos buffer docid=" << docid << " ords=" << posOrd << " through " << posOrd+tfreq-1 << std::endl;
          for (int i = posBufIdx; i < posBufEnd; i++) {
            posBuf[i] = posIS.readVint();
          }
        }
      }
    }
    // std::cout << "RETN pos buffer docid=" << docid << " posOrd=" << posOrd << " posBufIdx=" << posBufIdx << std::endl;
    auto delta = posBuf[posBufIdx++];
    // We could possibly move posOrd update and only update at block end.
    // Would need to adjust/account for where we started in a block though.
    // And given that there is no data dependency in this hot loop, it's unclear if it would help at all.
    posOrd++;
    pos += delta;
    return pos;
  }

  int32_t advancePosition(int32_t target) {
    while (pos < target) {
      nextPosition();
    }
    return pos;
  }

  // We could also think about exposing the position deltas?
  // Or, we could sum the positions in a block (for a doc) and then it would make bulk access
  // easier / more efficient.
  // Also think about bulk copying of positions when merging?  Would only work for first segment unless
  // we supported multiple position lists.  Hence prob only worth it when merging small segment into large.
  // How to do position skipping?  Could always pre-pend last position in a block?
};

}