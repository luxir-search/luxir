#pragma once

#include "solux/index/PostingsWriter.h"
#include "solux/store/Directory.h"
#include "solux/store/InputStream.h"

// TODO: we could probably improve compile times by pulling some needed constants out into a Postings header that both
// writer and reader could share?


// TODO: should the PostingsReader class be determining what files to open, or should a higher level class determine
// that and pass the opened files?  For now, assume the latter.


class PostingsReader;
class TermIndexReader;
class TermsEnum;
class DocsEnum;


// Lowest level postings reader class that needs to correspond to the PostingsWriter class that created the data.
// PostingsReader should be thread-safe at the top level, but any iterators it supplies would not be.
class PostingsReader {
public:

  InputFile* tindexFile;
  InputFile* termFile;
  InputFile* docFile;
  InputFile* posFile;

  PostingsReader(InputFile* tindexFile, InputFile* termFile, InputFile* docFile, InputFile* posFile)
  : tindexFile(tindexFile), termFile(termFile), docFile(docFile), posFile(posFile)
  {
  }

};



// not thread safe
class TermIndexReader {
  friend class TermsEnum;


  InputStream is;
  PostingsReader& postingsReader;

  PackedTerm fieldname;
  uint64_t termsOffset;
  uint64_t docsOffset;
  uint64_t posOffset;
  uint32_t nTerms;

  // actual index into terms
  uint32_t numTermBlocks;
  const uint64_t* termBlockOffsets;

  // TODO: field number?
public:
  TermIndexReader(MemPool& pool, PostingsReader& postingsReader) : postingsReader(postingsReader) {
    is = postingsReader.tindexFile->getInputStream();
  }

  // TODO: should this read into a different structure?
  void readNextField() {
    // See PostingsWriter.endField() for the format written.
    fieldname = is.readPackedTerm();
    termsOffset = is.readVlong();
    docsOffset = is.readVlong();
    posOffset = is.readVlong();
    nTerms = is.readVint();
    termBlockOffsets = reinterpret_cast<const uint64_t*>(is.ptr());
    numTermBlocks = ((nTerms-1) / PostingsWriter::TERMS_BLOCK_SIZE) + 1;
    is.skip(numTermBlocks * sizeof(uint64_t));
  }

  void readFieldAt(uint64_t offset) {
    is.seek(offset);
    readNextField();
  }


  TermIndexReader(PostingsReader& postingsReader,const InputStream& is) : postingsReader(postingsReader), is(is) {};
  TermRef name() { return fieldname; }
  int numTerms() { return nTerms; }  // TODO: move this down in hierarchy given that we don't know where it will be stored in the future?


  //  bool nextField(PostingsReader& reader);
  // void fillTermsEnum(PostingsReader& reader, TermsEnum& termsEnum); // TODO: should this be on TermsEnum or here?
};




class TermsEnum {
  friend class DocsEnum;

  InputStream is;
  PostingsReader& postingsReader;
  TermIndexReader& tindexReader;
  MemPool& pool;

  PackedTerm currTerm;
  int32_t ordInBlock = -1; // TODO: make ord 1-based everywhere and use 0 for "missing", unset, etc
  uint32_t docsSize;
  uint32_t pulsedDoc;
  uint32_t pulsedPos;

  // block-level information

  PackedTerm startingTerm;
  int32_t startingOrd = 0;
  int32_t maxOrdInBlock = -1;
  uint64_t offsetOfDocsForTermBlock;
  uint64_t offsetOfPositionsForTermBlock;
  uint64_t cumulativeDocsSize;

public:
  TermsEnum(MemPool& pool, PostingsReader& postingsReader, TermIndexReader& tindexReader) : pool(pool), postingsReader(postingsReader), tindexReader(tindexReader) {
    is = postingsReader.termFile->getInputStream();
    currTerm = PackedTerm(pool.allocatePtr(256));
  }

  int32_t ord() const {
    return startingOrd + ordInBlock;
  }

  PackedTerm term() const {
    return currTerm;
  }

  // read the data that comes after each term
  void readTermMetadata() {
    // see PostingsWriter.flushTerms
    docsSize = is.readVint();
    cumulativeDocsSize += docsSize;
    if (docsSize == 0) {
      pulsedDoc = is.readVint();
      pulsedPos = is.readVint();
    }
  }

  void readTermBlock() {
    if (ordInBlock == -1) {
      // first block we are reading, so seek to the first block.
      is.seek(tindexReader.termsOffset);
    } else {
      startingOrd += PostingsWriter::TERMS_BLOCK_SIZE;
    }
    cumulativeDocsSize = 0;
    ordInBlock = 0;
    maxOrdInBlock = std::min((int)PostingsWriter::TERMS_BLOCK_SIZE - 1, tindexReader.numTerms() - startingOrd - 1);

    // see PostingsWriter.flushTerms
    startingTerm = is.readPackedTerm();
    offsetOfDocsForTermBlock = is.readVlong();
    offsetOfPositionsForTermBlock = is.readVlong();

    memcpy(currTerm.ptr(), startingTerm.ptr(), startingTerm.memorySize());
    readTermMetadata();
  }

  bool nextTerm() {
    if (ordInBlock == maxOrdInBlock) {
      if (ord() + 1 >= tindexReader.numTerms()) {
        return false;
      }
      readTermBlock();
      return true;
    } else {
      ordInBlock++;
    }

    // read next suffix
    // see PostingsWriter.flushTerms
    uint8_t code = is.readByte();
    auto prefixLen = code >> 5;
    auto suffixLen = (code & 0x1f) + 1;
    if (prefixLen == 7) {
      prefixLen = is.readByte();
    }
    if (suffixLen == 31) {
      suffixLen = is.readVint()+32;
    }

    auto [data, len] = currTerm.unpack();
    is.read(const_cast<char*>(data+prefixLen), suffixLen);
    currTerm.setSize(prefixLen + suffixLen);

    readTermMetadata();
  }

  // TODO: a push interface that can more quickly/directly handle pulsed postings while allowing inlining?
  // That could also handle differences between block and doc
};


class DocsEnum {
  InputStream docIs;
  InputStream posIs;
  TermsEnum& tenum;
  MemPool& pool;
  int32_t docfreq; // number of docs containing this term
  uint64_t ttf;  // totalTermFreq

  int32_t docid;
  int32_t pos;
  int32_t tfreq;
  int64_t cumulativeTermFreq;  // to keep track of how many positions need to be skipped.
  int32_t ordStartBlock = 0;
  int32_t ordInBlock;
  uint64_t posOffset;
  uint64_t posIdx = 0;
  uint64_t posIdxStart = 0;

  int32_t docsSize;  // size of the postings in the doc file for the given term
  uint64_t statOfDocs;

  uint64_t cumulativeDocsSize;
  uint64_t offsetOfDocsForTermBlock;
  uint64_t offsetOfPositionsForTermBlock;

public:
  DocsEnum(MemPool& pool, PostingsReader& postingsReader, TermIndexReader& tindexReader, TermsEnum& tenum) : pool(pool), tenum(tenum) {
    // Since the same terms enum will often be used for multiple docs enum, we should copy everything we need
    // from the terms enum that we need (that may change.)
    // TODO: package those dependencies in a struct that can be simply assigned?  Or if there is enough overlap, simply copy the complete tenum?
    // Or we could invert the responsibility and make the client copy the tenum if they are going to change it.
    docsSize = tenum.docsSize;
    if (docsSize == 0) {
      // postings pulsed
      docid = tenum.pulsedDoc;
      pos = tenum.pulsedPos;
      docfreq = 1;
      ttf = 1;
    } else {
      offsetOfDocsForTermBlock = tenum.offsetOfDocsForTermBlock;
      offsetOfPositionsForTermBlock = tenum.offsetOfDocsForTermBlock;
      cumulativeDocsSize = tenum.cumulativeDocsSize;
      docIs = postingsReader.docFile->getInputStream();
      // see the end of PostingsWiter.endTerm() for the term-specific metadata written there (docfreq, ttf, etc)

      // read last byte of docs to get the metadata size
      docIs.seek(offsetOfDocsForTermBlock + cumulativeDocsSize - 1);
      uint8_t metaSize = docIs.readByte();
      docIs.relativeSeek(-metaSize - 1); // move to start of metadata
      docfreq = docIs.readVint();
      ttf = docfreq + docIs.readVlong();
      posOffset = docIs.readVlong();

      // start of the actual docs is end of block - size
      statOfDocs = offsetOfDocsForTermBlock + cumulativeDocsSize - docsSize;
      docIs.seek(statOfDocs);

      posIs = postingsReader.posFile->getInputStream();
      posIs.seek(offsetOfPositionsForTermBlock);
      cumulativeTermFreq = 0;
    }
  }

  int32_t numDocs() {
    return docfreq;
  }

  int32_t totalTermFreq() {
    return ttf;
  }

  int32_t nextDoc() {
    if (docsSize != 0) {
      // see PostingsWriter.endTerm() for format of non-block encoded docs/freqs
      uint32_t doccode = docIs.readVint();
      if ((doccode & 0x01)==1) {
        tfreq = 1;
      } else {
        tfreq = docIs.readVint();
      }
      posIdxStart = cumulativeTermFreq;
      cumulativeTermFreq += tfreq;
      docid = doccode >> 1;
      pos = 0;  // reset positions
    }
    return docid;
  }

  int32_t ord() {
    return ordStartBlock;
  }

  int32_t termFreq() {
    return tfreq;
  }

  void startPositions() {
    while (posIdx < posIdxStart) {
      // need to skip positions
      // TODO: a faster skipVint (potentially)? inlining may already eliminate the dead code though.
      auto delta = posIs.readVint();
    }
  }

  int32_t nextPosition() {
    if (docsSize != 0) {
      auto delta = posIs.readVint();
      pos += delta;
    }
    return pos;
  }

};


