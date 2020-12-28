#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
#include "gtest/gtest.h"
#include "SoluxTest.h"
#include<boost/container/static_vector.hpp>


class PostingsTest : public SoluxTest {
protected:
  RAMDir dir;
  MemPool pool;
  std::unique_ptr<PostingsWriter> writer;
  std::string field;
  std::string term;


  // set these limits lower for easier debugging
  uint32_t positionsPerDocMax=10;  // TODO: We don't have support for reading blocks yet, so make sure positionsPerDocMax*docsPerTermMax is less than a positions block size
  uint32_t docsPerTermMax=10;
  uint32_t termsPerFieldMax=100;  // TODO: stick to a single block for now


  std::unique_ptr<PostingsReader> reader;
  std::unique_ptr<TermIndexReader> tindexReader;
  std::unique_ptr<TermsEnum> tenum;
  std::unique_ptr<DocsEnum> docsEnum;

  rng_type rng_start;


  PostingsTest() {

  }

  void initWriter() {
    writer = std::make_unique<PostingsWriter>(dir,"gen1");

    // save the RNG state
    rng_start = rng;
  }

  void initReader() {
    writer->finish();

    auto tindexFile = dir.openFile("tindex");
    auto termFile = dir.openFile("term");
    auto docFile = dir.openFile("doc") ;
    auto posFile = dir.openFile("pos");
    reader = std::make_unique<PostingsReader>(tindexFile.get(), termFile.get(), docFile.get(), posFile.get());
    tindexReader = std::make_unique<TermIndexReader>(pool, *reader);

    // restore the RNG state
    rng = rng_start;
  }

  uint32_t getPositionDelta(int nPositions) {
    // TODO: do a better job at testing boundaries
    return rng.rint(1,(INT_MAX-1)/nPositions);
  }

  uint32_t getDocDelta(int nDocs) {
    // TODO: do a better job at testing boundaries
    return rng.rint(1,(INT_MAX-1)/nDocs);
  }

  uint32_t getNumPositions(uint32_t numDocs) {
    unused(numDocs);
    return rng.rint(1u, positionsPerDocMax);
  }

  uint32_t getNumDocs(uint32_t numTerms) {
    unused(numTerms);
    return rng.rint(1u,docsPerTermMax);
  }

  uint32_t getNumTerms(uint32_t numFields) {
    unused(numFields);
    return rng.rint(1u, termsPerFieldMax);
  }


  // numPositions is changed to the actual number indexed (random positions can overflow max)
  void addDoc(bool read, int docid, uint32_t& numPositions) {
    uint32_t readTf = 0;
    if (read) {
      auto readid = docsEnum->nextDoc();
      ASSERT_EQ(docid, readid);
      readTf = docsEnum->termFreq();  // read the tf first, but don't compare it until later
      docsEnum->startPositions();
    } else {
      writer->startDoc(docid);
    }
    uint64_t position = 0;
    uint32_t actualPositions = 0;
    for (uint32_t i=0; i<numPositions; i++) {   // TODO: introduce constants for limits
      auto delta = getPositionDelta(numPositions);
      position += delta;
      if (position >= INT_MAX) {
        break;
      }
      actualPositions++;
      if (read) {
        auto pos = docsEnum->nextPosition();
        // std::cout << "\t\tread pos=" << pos << std::endl;
        ASSERT_EQ(position, pos);
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


  void addTerm(bool read, const std::string& term, uint32_t numDocs) {
    TermRef termRef;
    uint32_t numDocsRead = 0;
    if (read) {
      if (numDocs > 0) {
        ASSERT_TRUE(tenum->nextTerm());
        ASSERT_EQ(tenum->term(), term);
        docsEnum = std::make_unique<DocsEnum>(pool, *reader, *tindexReader, *tenum);
        numDocsRead = docsEnum->numDocs();
      }
    }
    else {
      termRef = TermRef(pool, term.data(), term.size());
      writer->startTerm(termRef);
    }
    uint64_t docid = 0;
    int actualDocs = 0;
    uint64_t actualttf = 0;
    for (uint32_t i=0; i<numDocs; i++) {
      auto docDelta = getDocDelta(numDocs);
      docid += docDelta;
      if (docid > INT_MAX) {
        break;
      }
      actualDocs++;
      uint32_t numPositions = getNumPositions(numDocs);
      addDoc(read, (int)docid, numPositions);
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

  void addField(bool read, const std::string& fname, uint32_t numTerms) {
    std::string term = "term";
    term.resize(12);

    if (read) {
      tindexReader->readNextField();
      ASSERT_EQ(fname,tindexReader->name());
      tenum = std::make_unique<TermsEnum>(pool, *reader, *tindexReader);
    } else {
      writer->startField(fname);
    }
    int realNumTerms = 0;
    for (uint32_t i=0; i<numTerms; i++) {
      // std::format not implemented yet...
      sprintf(term.data()+4,"%08d",i);
      auto ndocs = getNumDocs(numTerms);
      if (ndocs > 0) ++realNumTerms;  // if number of docs for term ends up being 0, we should drop the term.
      addTerm(read, term, ndocs);
    }
    if (read) {
      ASSERT_EQ(tindexReader->numTerms(), realNumTerms);
    }else {
      writer->endField(fname);
    }
  }

  void addFields(bool read, uint32_t numFields) {
    std::string fname = "field";
    fname.resize(13);

    for (uint32_t i=0; i<numFields; i++) {
      // std::format not implemented yet...
      sprintf(fname.data()+5,"%08d",i);
      addField(read, fname, getNumTerms(numFields));
    }
  }

  int stackfill(uint64_t fill, int sz) {
    uint64_t* p = (uint64_t*)alloca(sz*sizeof(uint64_t));
    uint64_t ret = rng();
    for (int i=0; i<sz; i++) {
      p[i] = rng();
    }
    // conspire to set all the mem to the same thing without the compiler optimizing it away
    for (int i=0; i<sz; i++) {
      uint64_t otherIdx = rng() % sz;
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
  PostingsWriter writer(dir, "gen1");
  std::string t1 = "term1";
  std::string t2 = "term2";
  std::string ta = "termA";
  TermRef term1(pool,t1.data(),t1.size());
  TermRef term2(pool,t2.data(),t2.size());
  TermRef terma(pool,ta.data(),ta.size());

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


  auto tindexFile = dir.openFile("tindex");
  auto termFile = dir.openFile("term");
  auto docFile = dir.openFile("doc") ;
  auto posFile = dir.openFile("pos");
  PostingsReader reader(tindexFile.get(), termFile.get(), docFile.get(), posFile.get());

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
        docsEnum.startPositions();
        for (int j = 0; j < tfreq; j++) {
          auto pos = docsEnum.nextPosition();
          std::cout << "\t\t\t\tpos=" << pos << std::endl;
        }
      }
    }
  }

}



TEST_F(PostingsTest, randWrite) {
  std::cout << "SEED=" << rng_seed << std::endl;
  initWriter();
  addFields(false,10);
  initReader();
  addFields(true,10);
}
