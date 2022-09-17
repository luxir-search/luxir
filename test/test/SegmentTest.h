#pragma once
#include <tuple>
#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
#include "solux/util/random.h"
#include "gtest/gtest.h"

namespace solux {

// create and test a random segment
class SegmentTest {
public:
  RAMDir dir;
  MemPool pool;
  MemPool::save_point save = pool.getSavePoint();
  std::unique_ptr<PostingsWriter> postingsWriter;
  std::unique_ptr<TextWriter> writer;
  std::string field;
  std::string term;


  // set these limits lower for easier debugging
  uint32_t positionsPerDocMax = 10;  // TODO: We don't have support for reading blocks yet, so make sure positionsPerDocMax*docsPerTermMax is less than a positions block size
  uint32_t docsPerTermMax = 10;
  uint32_t termsPerFieldMax = 100;  // TODO: stick to a single term block for now
  int32_t highestDoc = -1;

  std::unique_ptr<PostingsReader> reader;
  std::unique_ptr<FieldReader> fieldReader;
  std::unique_ptr<TermsEnum> tenum;
  std::unique_ptr<DocsEnum> docsEnum;

  Rng r;
  Rng r2;
  Rng rng_snapshot;

  uint64_t fingerprint = 0;  // sum of all docs and positions calculated when writing
  uint64_t indexSize = 0;

  static void makeTerm(int termNum, std::string& target) {
    target.resize(12);
    memcpy(target.data(), "term", 4);
    sprintf(target.data() + 4, "%08d", termNum);
  }


  SegmentTest() {
  }

  uint64_t getIndexSize() {
    return indexSize;
  }

  void initWriter() {
    dir = RAMDir();  // remove all files?

    docsEnum.reset();
    tenum.reset();
    fieldReader.reset();
    writer.reset();
    reader.reset();

    pool.rewind(save);

    fingerprint = 0;

    highestDoc = -1;
    postingsWriter = std::make_unique<PostingsWriter>(dir, "10", 0x7fffffff);  // use maximum value for maxDoc... nothing (currently) in text field depends on it.
    writer = std::make_unique<TextWriter>(*postingsWriter);  // use maximum value for maxDoc... nothing (currently) in text field depends on it.

    // save the RNG state
    rng_snapshot = r;
    // re-init secondary rng off of first
    r2.init(r());
  }

  void initReader() {
    postingsWriter->finish();

    reader = std::make_unique<PostingsReader>(dir, "10");
    fieldReader = std::make_unique<FieldReader>(pool, *reader);

    // restore the RNG state
    r = rng_snapshot;
    // re-init secondary rng off of first
    r2.init(r());

    std::vector<std::string> files;
    dir.listFiles(files);
    indexSize = 0;
    for (auto& fname : files) {
      indexSize += dir.openFile(fname)->size();
    }

    // TODO: refactor this somewhere more useful.  Directory?
    // std::cout << "INDEX SIZE tif=" << tindexFile->size() << " tf=" << termFile->size() << " df=" << docFile->size() << " pf=" << posFile->size() << std::endl;
  }

  // make a new terms enum .tenum from the current fieldReader... a must if the fieldReader has changed states
  void makeTermsEnum() {
    tenum = std::make_unique<TermsEnum>(pool, *reader, *fieldReader);
  }

  uint32_t getPositionDelta(int nPositions) {
    // TODO: do a better job at testing boundaries
    return r.rint(1, (INT_MAX - 1) / nPositions);
  }

  uint32_t getDocDelta(int nDocs) {
    // TODO: do a better job at testing boundaries
    return r.rint(1, (INT_MAX - 1) / nDocs);
  }

  uint32_t getNumPositions(uint32_t numDocs) {
    unused(numDocs);
    return r.rint(1u, positionsPerDocMax);
  }

  uint32_t getNumDocs(uint32_t numTerms) {
    unused(numTerms);
    return r.rint(1u, docsPerTermMax);
  }

  uint32_t getNumTerms(uint32_t numFields) {
    unused(numFields);
    return r.rint(1u, termsPerFieldMax);
  }

  void getFieldName(std::string& target, uint32_t fieldNum) {
    target.resize(13);
    sprintf(term.data(), "field%08d", fieldNum);
  }


  // numPositions is changed to the actual number indexed (random positions can overflow max)
  void addDoc(bool read, int docid, uint32_t &numPositions) {
    uint32_t readTf = 0;
    uint32_t maxRead = numPositions;
    if (read) {
      auto readid = docsEnum->nextDoc();
      ASSERT_EQ(docid, readid);
      readTf = docsEnum->termFreq();  // read the tf first, but don't compare it until later
      if (r2.rbool()) {
        // don't read all of the positions
        maxRead = 0;  // for now, don't read any (simplate skipping)
      }
      if (maxRead > 0 || r2.rbool()) { // sometimes call startPositions even if we aren't going to read positions
        docsEnum->startPositions();
      }
    } else {
      writer->startDoc(docid);
    }
    uint64_t position = 0;
    uint32_t actualPositions = 0;
    for (uint32_t i = 0; i < numPositions; i++) {   // TODO: introduce constants for limits
      auto delta = getPositionDelta(numPositions);
      position += delta;
      if (position >= INT_MAX) {
        break;
      }
      actualPositions++;
      if (read) {
        if (maxRead > 0) {
          maxRead--;
          auto pos = docsEnum->nextPosition();
          // std::cout << "\t\tread pos=" << pos << std::endl;
          ASSERT_EQ(position, pos);
        }
      } else {
        // std::cout << "\t\tadding posDelta=" << delta << " pos=" << position << std::endl;
        writer->addPositionDelta(delta);
        fingerprint += position;
        // std::cout << "P fingerprint+=" << position << " total=" << fingerprint << std::endl;
      }
    }
    if (read) {
      ASSERT_EQ(readTf, actualPositions);
      // undefined behavior reading positions past termFreq
    } else {
      writer->endDoc(docid);
    }
    numPositions = actualPositions;
  }


  void addTerm(bool read, const std::string &term, uint32_t numDocs, int64_t nPos=-1) {
    TermRef termRef;
    uint32_t numDocsRead = 0;
    if (read) {
      if (numDocs > 0) {
        ASSERT_TRUE(tenum->nextTerm());
        ASSERT_EQ(tenum->term(), term);
        docsEnum = std::make_unique<DocsEnum>(pool, *reader, *tenum);
        numDocsRead = docsEnum->numDocs();
      }
    } else {
      termRef = TermRef(pool, term.data(), term.size());
      writer->startTerm(termRef);
    }
    uint64_t docid = 0;
    int actualDocs = 0;
    uint64_t actualttf = 0;
    for (uint32_t i = 0; i < numDocs; i++) {
      auto docDelta = getDocDelta(numDocs);
      if (docid + docDelta > INT_MAX) {
        break;
      }
      docid += docDelta;
      actualDocs++;
      uint32_t numPositions = nPos<0 ? getNumPositions(numDocs) : (uint32_t)nPos;
      addDoc(read, (int) docid, numPositions);
      actualttf += numPositions;
      if (!read && numPositions > 0) {
        fingerprint += docid;
        // std::cout << "D fingerprint+=" << docid << " total=" << fingerprint << std::endl;
      }
    }
    highestDoc = std::max(highestDoc, (int)docid);
    if (read) {
      if (numDocs > 0) {
        ASSERT_EQ(actualDocs, numDocsRead);
        ASSERT_EQ(actualttf, docsEnum->totalTermFreq());
      }
    } else {
      writer->endTerm(termRef);
    }
  }

  void addField(bool read, const std::string &fname, uint32_t numTerms, int64_t nDocs=-1, int64_t nPos=-1) {
    std::string term = "term";
    term.resize(12);

    if (read) {
      fieldReader->readNextField();
      ASSERT_EQ(fname, fieldReader->name());
      makeTermsEnum();
    } else {
      writer->startField(fname);
    }
    int realNumTerms = 0;
    for (uint32_t i = 0; i < numTerms; i++) {
      // std::format not implemented yet...
      sprintf(term.data() + 4, "%08d", i);
      auto ndocs = nDocs<0 ? getNumDocs(numTerms) : (uint32_t)nDocs;
      if (ndocs > 0) ++realNumTerms;  // if number of docs for term ends up being 0, we should drop the term.
      addTerm(read, term, ndocs, nPos);
    }
    if (read) {
      ASSERT_EQ(fieldReader->numTerms(), realNumTerms);
    } else {
      writer->endField(fname);
    }
  }

  // -1 means a random number is used per field/term/doc
  void addFields(bool read, uint32_t numFields, int64_t nTerms=-1, int64_t nDocs=-1, int64_t nPos=-1) {
    std::string fname;

    for (uint32_t i = 0; i < numFields; i++) {
      getFieldName(fname, i);
      uint32_t numTerms = nTerms<0 ? getNumTerms(numFields) : (uint32_t)nTerms;
      addField(read, fname, numTerms, nDocs, nPos);
    }
  }

  // return fingerprint, #terms read, #docs read, #positions read
  std::tuple<int64_t,int64_t,int64_t,int64_t> readFingerprint(int percentReadPositions) {
    int64_t totTerms = 0;
    int64_t totDocs = 0;
    int64_t totPositions = 0;
    int64_t ret = 0;
    FieldReader fieldReader(pool, *reader);
    while (fieldReader.readNextField()) {
      TermsEnum tenum(pool, *reader, fieldReader);
      while (tenum.nextTerm()) {
        totTerms++;
        DocsEnum docsEnum(pool, *reader, tenum);
        auto ndocs = docsEnum.numDocs();
        for (int i = 0; i < ndocs; i++) {
          auto id = docsEnum.nextDoc();
          ret += id;
          totDocs++;
          // std::cout << "d fingerprint+=" << id << " total=" << ret << std::endl;

          bool readPositions = percentReadPositions>0;  // todo: impl percentages

          if (readPositions) {
            auto tfreq = docsEnum.termFreq();
            docsEnum.startPositions();
            for (int j = 0; j < tfreq; j++) {
              auto pos = docsEnum.nextPosition();
              ret += pos;
              totPositions++;
              // std::cout << "p fingerprint+=" << pos << " total=" << ret << std::endl;
            }
          }

        }
      }
    }
    return {ret, totTerms, totDocs, totPositions};
  }


};

} // end namespace