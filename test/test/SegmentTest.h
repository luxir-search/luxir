#pragma once
#include <tuple>
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/PosEnum.h"
#include "luxir/util/random.h"
#include "gtest/gtest.h"

namespace luxir {

// Create and test a random segment
// This is old code that was used to test low level reading and writing before there was higher level
// functionality like Inverter and IndexHandlers. This should no longer be used for new tests.
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
  std::unique_ptr<DocsPosEnum> docsEnum;
  std::unique_ptr<PosEnum> posEnum;

  Rng r;
  Rng r2;
  Rng rng_snapshot;

  uint64_t fingerprint = 0;  // sum of all docs and positions calculated when writing
  uint64_t indexSize = 0;

  // terms are added in order (and not resorted), so the term must sort according to the term number.
  static void makeTerm(int termNum, std::string& target) {
    // prefix lengths less than 7 and suffix lengths less than 32 are encoded in a single byte.
    SplitMix64 localRng(termNum);
    auto code = localRng();

    bool moreSuffix = ((code) & 0x03) == 0;  // 1/4th of the time add more suffix
    code >>= 4;
    auto slen = moreSuffix ? code & 0x3f : 0;  // extra suffix to 63
    code >>= 8;


    std::string_view data = "now is the time for all good men to come to the aid of their country.";
    assert(data.size() >= 63);
    target.clear();
    target.reserve( slen + 12);

    char buf[20];
    sprintf(buf, "term%08d", termNum);
    target.append(buf);

    if (moreSuffix) {
      target.append(data.data(), slen);
    }
  }


  SegmentTest() {
  }

  uint64_t getIndexSize() {
    return indexSize;
  }

  void initWriter() {
    dir = RAMDir();  // remove all files?

    posEnum.reset();
    docsEnum.reset();
    tenum.reset();
    fieldReader.reset();
    writer.reset();
    reader.reset();

    pool.rewind(save);

    fingerprint = 0;

    highestDoc = -1;
    postingsWriter = std::make_unique<PostingsWriter>(dir, 0, 0x7fffffff);  // use maximum value for numDocs... nothing (currently) in text field depends on it.
    // writer = std::make_unique<TextWriter>(*postingsWriter);  // use maximum value for numDocs... nothing (currently) in text field depends on it.

    // save the RNG state
    rng_snapshot = r;
    // re-init secondary rng off of first
    r2.init(r());
  }

  void initReader() {
    postingsWriter->finish();

    reader = std::make_unique<PostingsReader>(dir, 0);
    fieldReader = std::make_unique<FieldReader>(*reader);

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

  // make a new terms enum .tenum from the current fieldReader... only call if fieldReader has changed states
  SegFieldInfo fieldInfo;
  void makeTermsEnum() {
    fieldReader->readFieldInfo(fieldInfo);
    tenum = std::make_unique<TermsEnum>(pool, *reader, fieldInfo);
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
        posEnum->startPositions();
      }
    } else {
      writer->startDoc(docid);
    }
    int64_t position = -1;
    int32_t actualPositions = 0;
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
          auto pos = posEnum->nextPosition();
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
        docsEnum = std::make_unique<DocsPosEnum>(*tenum);
        posEnum = std::make_unique<PosEnum>(*docsEnum);
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
    std::string term;

    if (read) {
      ASSERT_TRUE(fieldReader->readNextField());
      ASSERT_EQ(fname, fieldReader->name());
      makeTermsEnum();
    } else {
      writer = std::make_unique<TextWriter>(*postingsWriter);
      writer->startField(fname);
    }
    int realNumTerms = 0;
    for (uint32_t i = 0; i < numTerms; i++) {
      makeTerm(i, term);
      auto ndocs = nDocs<0 ? getNumDocs(numTerms) : (uint32_t)nDocs;
      if (ndocs > 0) ++realNumTerms;  // if number of docs for term ends up being 0, we should drop the term.
      addTerm(read, term, ndocs, nPos);
    }
    if (read) {
      ASSERT_EQ(tenum->numTerms(), realNumTerms);
    } else {
      writer->endField();
      writer.reset();
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
    auto read = [&]<DocsEnumTier Tier>() {
      int64_t totTerms = 0;
      int64_t totDocs = 0;
      int64_t totPositions = 0;
      int64_t ret = 0;
      FieldReader fieldReader(*reader);
      SegFieldInfo fieldInfo;
      while (fieldReader.readNextField()) {
        fieldReader.readFieldInfo(fieldInfo);
        TermsEnum tenum(pool, *reader, fieldInfo);
        while (tenum.nextTerm()) {
          totTerms++;
          BasicDocsEnum<Tier> docsEnum(tenum);
          std::unique_ptr<PosEnum> posEnum;
          if constexpr (Tier == DocsEnumTier::POSITIONS) {
            posEnum = std::make_unique<PosEnum>(docsEnum);
          }
          auto ndocs = docsEnum.numDocs();
          for (int i = 0; i < ndocs; i++) {
            auto id = docsEnum.nextDoc();
            ret += id;
            totDocs++;
            if constexpr (Tier == DocsEnumTier::POSITIONS) {
              auto tfreq = docsEnum.termFreq();
              posEnum->startPositions();
              for (int j = 0; j < tfreq; j++) {
                ret += posEnum->nextPosition();
                totPositions++;
              }
            }
          }
        }
      }
      return std::tuple{ret, totTerms, totDocs, totPositions};
    };
    return percentReadPositions > 0
        ? read.template operator()<DocsEnumTier::POSITIONS>()
        : read.template operator()<DocsEnumTier::DOCS>();
  }


};

} // end namespace
