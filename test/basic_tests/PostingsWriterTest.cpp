#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include<boost/container/static_vector.hpp>

namespace solux {

class PostingsTest : public SoluxTest {
protected:
  RAMDir dir;
  MemPool pool;
  MemPool::save_point save = pool.getSavePoint();
  std::unique_ptr<PostingsWriter> writer;
  std::string field;
  std::string term;


  // set these limits lower for easier debugging
  uint32_t positionsPerDocMax = 10;
  uint32_t docsPerTermMax = 10;
  uint32_t termsPerFieldMax = 100;


  std::unique_ptr<PostingsReader> reader;
  std::unique_ptr<TermIndexReader> tindexReader;
  std::unique_ptr<TermsEnum> tenum;
  std::unique_ptr<DocsEnum> docsEnum;

  Rng rng_start;
  Rng r2;


  PostingsTest() {
  }

  void initWriter() {
    pool.rewind(save);
    dir = RAMDir();  // remove all files?
    writer = std::make_unique<PostingsWriter>(dir, "10");

    // save the RNG state
    rng_start = rng;
    // re-init secondary rng off of first
    r2.init(rng());
  }

  void initReader() {
    writer->finish();

    std::string gen = "10";
    reader = std::make_unique<PostingsReader>(dir, "10");
    tindexReader = std::make_unique<TermIndexReader>(pool, *reader);

    // restore the RNG state
    rng = rng_start;
    // re-init secondary rng off of first
    r2.init(rng());
  }

  uint32_t getPositionDelta(int nPositions) {
    // TODO: do a better job at testing boundaries
    return rng.rint(1, (INT_MAX - 1) / nPositions);
  }

  uint32_t getDocDelta(int nDocs) {
    // TODO: do a better job at testing boundaries
    return rng.rint(1, (INT_MAX - 1) / nDocs);
  }

  uint32_t getNumPositions(uint32_t numDocs) {
    unused(numDocs);
    return rng.rint(1u, positionsPerDocMax);
  }

  uint32_t getNumDocs(uint32_t numTerms) {
    unused(numTerms);
    return rng.rint(1u, docsPerTermMax);
  }

  uint32_t getNumTerms(uint32_t numFields) {
    unused(numFields);
    return rng.rint(1u, termsPerFieldMax);
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


  void addTerm(bool read, const std::string &term, uint32_t numDocs) {
    TermRef termRef;
    termRef.init(nullptr,0);  // just to get rid of "possibly uninitialized" warning

    uint32_t numDocsRead = 0;
    if (read) {
      if (numDocs > 0) {
        ASSERT_TRUE(tenum->nextTerm());
        ASSERT_EQ(tenum->term(), term);
        docsEnum = std::make_unique<DocsEnum>(pool, *reader, *tindexReader, *tenum);
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
      docid += docDelta;
      if (docid > INT_MAX) {
        break;
      }
      actualDocs++;
      uint32_t numPositions = getNumPositions(numDocs);
      addDoc(read, (int) docid, numPositions);
      actualttf += numPositions;
    }
    if (read) {
      if (numDocs > 0) {
        ASSERT_EQ(actualDocs, numDocsRead);
        ASSERT_EQ(actualttf, docsEnum->totalTermFreq());
      }
    } else {
      writer->endTerm(termRef);
    }
  }

  void addField(bool read, const std::string &fname, uint32_t numTerms) {
    std::string term = "term";
    term.resize(12);

    if (read) {
      tindexReader->readNextField();
      ASSERT_EQ(fname, tindexReader->name());
      tenum = std::make_unique<TermsEnum>(pool, *reader, *tindexReader);
    } else {
      writer->startField(fname);
    }
    int realNumTerms = 0;
    for (uint32_t i = 0; i < numTerms; i++) {
      // std::format not implemented yet...
      sprintf(term.data() + 4, "%08d", i);
      auto ndocs = getNumDocs(numTerms);
      if (ndocs > 0) ++realNumTerms;  // if number of docs for term ends up being 0, we should drop the term.
      addTerm(read, term, ndocs);
    }
    if (read) {
      ASSERT_EQ(tindexReader->numTerms(), realNumTerms);
    } else {
      writer->endField(fname);
    }
  }

  void addFields(bool read, uint32_t numFields) {
    std::string fname = "field";
    fname.resize(13);

    for (uint32_t i = 0; i < numFields; i++) {
      // std::format not implemented yet...
      sprintf(fname.data() + 5, "%08d", i);
      addField(read, fname, getNumTerms(numFields));
    }
  }

  int stackfill(uint64_t fill, int sz) {
    Rng r;
    uint64_t *p = (uint64_t *) alloca(sz * sizeof(uint64_t));
    uint64_t ret = r();
    for (int i = 0; i < sz; i++) {
      p[i] = r();
    }
    // conspire to set all the mem to the same thing without the compiler optimizing it away
    for (int i = 0; i < sz; i++) {
      uint64_t otherIdx = r() % sz;
      if (otherIdx != 7) {
        p[i] = fill;
      }
      ret += p[otherIdx];
    }
    return ret;
  }

};


TEST_F(PostingsTest, basic) {
  RAMDir dir;
  MemPool pool;
  PostingsWriter writer(dir, "10");
  std::string t1 = "term1";
  std::string t2 = "term2";
  std::string ta = "termA";
  TermRef term1(pool, t1.data(), t1.size());
  TermRef term2(pool, t2.data(), t2.size());
  TermRef terma(pool, ta.data(), ta.size());

  writer.startField("field1");
  writer.startTerm(term1);  // single doc, single position... this should be pulsed
  writer.startDoc(44);
  writer.addPositionDelta(1);
  writer.endDoc(44);
  writer.startDoc(55);
  writer.addPositionDelta(555);
  writer.addPositionDelta(111);
  writer.endDoc(55);
  writer.startDoc(56);
  writer.addPositionDelta(7);
  writer.addPositionDelta(2);
  writer.endDoc(56);
  writer.endTerm(term1);

  writer.startTerm(term2);
  writer.startDoc(7);
  writer.addPositionDelta(5);
  writer.addPositionDelta(3);
  writer.addPositionDelta(10);
  writer.endDoc(7);
  writer.startDoc(22);
  writer.addPositionDelta(0);
  writer.addPositionDelta(300);
  writer.endDoc(22);
  writer.endTerm(term2);

  writer.endField("field1");


  writer.startField("field2");
  writer.startTerm(terma);
  writer.startDoc(0);
  writer.addPositionDelta(3);
  writer.addPositionDelta(1);
  writer.endDoc(0);
  writer.endTerm(terma);
  writer.endField("field2");

  writer.finish();


  PostingsReader reader(dir, "10");

  TermIndexReader tindexReader(pool, reader);
  while (tindexReader.readNextField()) {
    std::cout << "FIELD NAME name=" << tindexReader.name() << " numTerms=" << tindexReader.numTerms() << std::endl;

    TermsEnum tenum(pool, reader, tindexReader);
    while (tenum.nextTerm()) {
      std::cout << "\tTERM=" << tenum.term() << " ord=" << tenum.ord() << std::endl;
      // if (tenum.ord()==0) continue; // skip first term, good for figuring out of second term errors are due to reader or writer.

      DocsEnum docsEnum(pool, reader, tindexReader, tenum);
      auto ndocs = docsEnum.numDocs();
      std::cout << "\t\tnumDocs=" << docsEnum.numDocs() << " totalTermFreq=" << docsEnum.totalTermFreq() << std::endl;

      for (int i = 0; i < ndocs; i++) {
        auto id = docsEnum.nextDoc();
        auto tfreq = docsEnum.termFreq();
        std::cout << "\t\t\tdocid=" << id << " termFreq=" << tfreq << std::endl;
        if (i==1) { continue; }  // test skipping reading a docs positions
        docsEnum.startPositions();
        for (int j = 0; j < tfreq; j++) {
          auto pos = docsEnum.nextPosition();
          std::cout << "\t\t\t\tpos=" << pos << std::endl;
        }
      }
    }
  }

}

TEST_F(PostingsTest, blockPositions) {
  RAMDir dir;
  MemPool pool;
  PostingsWriter writer(dir, "10");
  std::string t1 = "term1";
  TermRef term1(pool, t1.data(), t1.size());

  writer.startField("field1");
  writer.startTerm(term1);
  writer.startDoc(42);
  int nPos = Postings::POSITIONS_BLOCK_SIZE * 2 + 2; // TODO: parameterize
  int delta = 2;
  for (int i = 0; i < nPos; i++) {
    writer.addPositionDelta(delta);
  }
  writer.endDoc(42);

  writer.startDoc(43);
  int nPos2 = Postings::POSITIONS_BLOCK_SIZE * 2 + 2; // TODO: parameterize
  int delta2 = 3;
  for (int i = 0; i < nPos; i++) {
    writer.addPositionDelta(delta2);
  }
  writer.endDoc(43);


  writer.endTerm(term1);
  writer.endField("field1");
  writer.finish();

  PostingsReader reader(dir, "10");

  TermIndexReader tindexReader(pool, reader);
  ASSERT_TRUE(tindexReader.readNextField());
  std::cout << "FIELD NAME name=" << tindexReader.name() << " numTerms=" << tindexReader.numTerms() << std::endl;
  ASSERT_EQ(tindexReader.name(), std::string_view("field1"));
  ASSERT_EQ(tindexReader.numTerms(), 1);

  TermsEnum tenum(pool, reader, tindexReader);
  ASSERT_TRUE(tenum.nextTerm());
  ASSERT_EQ(tenum.ord(), 0);
  ASSERT_EQ(tenum.term(), std::string_view("term1"));

  DocsEnum docsEnum(pool, reader, tindexReader, tenum);
  ASSERT_EQ(docsEnum.numDocs(), 2);
  ASSERT_EQ(docsEnum.totalTermFreq(), nPos + nPos2);

  auto id = docsEnum.nextDoc();
  auto tfreq = docsEnum.termFreq();
  ASSERT_EQ(id, 42);
  ASSERT_EQ(tfreq, nPos);
  // if (false)  // skip reading positions
  {
    docsEnum.startPositions();
    uint32_t lastPos = 0;
    for (int i = 0; i < tfreq; i++) {
      auto pos = docsEnum.nextPosition();
      auto posDelta = pos - lastPos;
      lastPos = pos;
      ASSERT_EQ(posDelta, delta);
    }
  }

  auto id2 = docsEnum.nextDoc();
  auto tfreq2 = docsEnum.termFreq();
  ASSERT_EQ(id2, 43);
  ASSERT_EQ(tfreq2, nPos2);
  {
    docsEnum.startPositions();
    uint32_t lastPos = 0;
    for (int i = 0; i < tfreq; i++) {
      auto pos = docsEnum.nextPosition();
      auto posDelta = pos - lastPos;
      lastPos = pos;
      ASSERT_EQ(posDelta, delta2);
    }
  }

  ASSERT_FALSE(tenum.nextTerm());
  ASSERT_FALSE(tindexReader.readNextField());
}

TEST_F(PostingsTest, blockTerms) {
  RAMDir dir;
  MemPool pool;
  PostingsWriter writer(dir, "10");
  int nTerms = Postings::TERMS_BLOCK_SIZE + 1;

  writer.startField("field1");

  std::string tstr = "term";
  tstr.resize(12);
  for (int i=0; i<nTerms; i++) {
    sprintf(tstr.data() + 4, "%08d", i);
    TermRef term(pool, tstr.data(), tstr.size());
    writer.startTerm(term);
    writer.startDoc(i);
    writer.addPositionDelta(i*2);
    writer.addPositionDelta(1);
    writer.endDoc(i);
    writer.endTerm(term);
  }
  writer.endField("field1");
  writer.finish();

  PostingsReader reader(dir, "10");

  TermIndexReader tindexReader(pool, reader);
  ASSERT_TRUE(tindexReader.readNextField());
  // std::cout << "FIELD NAME name=" << tindexReader.name() << " numTerms=" << tindexReader.numTerms() << std::endl;
  ASSERT_EQ(tindexReader.name(), std::string_view("field1"));
  ASSERT_EQ(tindexReader.numTerms(), nTerms);

  TermsEnum tenum(pool, reader, tindexReader);
  for (int i=0; i<nTerms; i++) {
    sprintf(tstr.data() + 4, "%08d", i);

    ASSERT_TRUE(tenum.nextTerm());
    ASSERT_EQ(tenum.ord(), i);
    ASSERT_EQ(tenum.term(), tstr);


    DocsEnum docsEnum(pool, reader, tindexReader, tenum);
    ASSERT_EQ(docsEnum.numDocs(), 1);
    ASSERT_EQ(docsEnum.totalTermFreq(), 2);
    ASSERT_EQ(docsEnum.nextDoc(), i);
    ASSERT_EQ(docsEnum.termFreq(), 2);

    docsEnum.startPositions();
    ASSERT_EQ(docsEnum.nextPosition(), i*2);
    ASSERT_EQ(docsEnum.nextPosition(), i*2 + 1);
    ASSERT_EQ(docsEnum.nextPosition(), INT_MAX);  // TODO: replace with constant

    ASSERT_EQ(docsEnum.nextDoc(), INT_MAX);
  }

  ASSERT_FALSE(tenum.nextTerm());
  ASSERT_FALSE(tindexReader.readNextField());
}



TEST_F(PostingsTest, randTail) {
  // avoid creating a full block of terms, docs, or positions
  positionsPerDocMax = 11;
  docsPerTermMax = 11;
  termsPerFieldMax = 120;
  for (int i=0; i<10; i++) {
    auto nFields = rng.rint(1,20);
    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}

TEST_F(PostingsTest, randManyPos) {
  for (int i=0; i<10; i++) {
    auto nFields = rng.rint(1,20);
    positionsPerDocMax = Postings::POSITIONS_BLOCK_SIZE * 5/2;
    docsPerTermMax = 120;  // less than a doc block
    termsPerFieldMax = 2;

    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}

TEST_F(PostingsTest, randManyDocPos) {
  for (int i=0; i<10; i++) {
    auto nFields = rng.rint(1,20);
    positionsPerDocMax = Postings::POSITIONS_BLOCK_SIZE * 5/2;
    docsPerTermMax = Postings::DOCS_BLOCK_SIZE * 5/2;
    termsPerFieldMax = 2;

    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}

// test enough terms that more than one term block per field is required
TEST_F(PostingsTest, randManyTerms) {
  positionsPerDocMax = 3;
  docsPerTermMax = 3;
  termsPerFieldMax = Postings::TERMS_BLOCK_SIZE * 5 / 2;

  for (int i=0; i<10; i++) {
    auto nFields = rng.rint(1,20);
    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}

#if REMOVED
// An indispensable example of how to come up with a very small test case that fails.  Run many times with multiple seeds
// and if one fails, then set the lower bound of the loop to that seed number and debug!
TEST_F(PostingsTest, randWriteTmp) {
  std::cout << "SEED=" << rng_seed << std::endl;

  int nFields = 1;

  // this set is good for finding bugs with pulsed and skipping  (just 1 term, 1 or 2 docs, 1 or 2 positions)
  positionsPerDocMax = 3; docsPerTermMax = 3; termsPerFieldMax = 2;

  // this set is good for finding bugs with position blocks mixed in and skipping
  positionsPerDocMax = Postings::POSITIONS_BLOCK_SIZE*3/2; docsPerTermMax = 3; termsPerFieldMax = 2;

  // doc blocks
  positionsPerDocMax = 3; docsPerTermMax = Postings::DOCS_BLOCK_SIZE*2; termsPerFieldMax = 2;

  // term blocks
  positionsPerDocMax = 3; docsPerTermMax = 3; termsPerFieldMax = Postings::TERMS_BLOCK_SIZE*5/2;

  // multiple fields
  nFields=2;

  for (int i=1; i<1000; i++) {
    std::cout << "seed " << i << std::endl;
    rng.init(i);
    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}
#endif

} // end namespace