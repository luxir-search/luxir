#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
#include "gtest/gtest.h"
#include "SoluxTest.h"

class PostingsTest : public SoluxTest {
protected:
  RAMDir dir;
  MemPool pool;
  std::unique_ptr<PostingsWriter> writer;
  std::string field;
  std::string term;


  // set these limits lower for easier debugging
  uint32_t positionsPerDocMax=4;
  uint32_t docsPerTermMax=4;
  uint32_t termsPerFieldMax=4;


  PostingsTest() {

  }

  void init() {
    writer = std::make_unique<PostingsWriter>(dir,"gen1");
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
    return rng.rint(1u, positionsPerDocMax);
  }

  uint32_t getNumTerms(uint32_t numFields) {
    return rng.rint(1u, termsPerFieldMax);
  }

  uint32_t getNumDocs(uint32_t numTerms) {
    return rng.rint(1u,docsPerTermMax);
  }

  void addDoc(int docid, uint32_t numPositions) {
    writer->startDoc(docid);
    uint64_t position = 0;
    for (int i=0; i<numPositions; i++) {   // TODO: introduce constants for limits
      auto delta = getPositionDelta(numPositions);
      position += delta;
      if (position >= INT_MAX) {
        break;
      }
      writer->addPositionDelta(delta);
    }
    writer->endDoc(docid);
  }


  void addTerm(const std::string& term, uint32_t numDocs) {
    TermRef termRef(pool,term.data(),term.size());
    writer->startTerm(termRef);
    uint64_t docid = 0;
    for (int i=0; i<numDocs; i++) {
      auto docDelta = getDocDelta(numDocs);
      docid += docDelta;
      if (docid > INT_MAX) {
        break;
      }
      addDoc((int)docid, getNumPositions(numDocs));
    }
    writer->endTerm(termRef);
  }

  void addField(const std::string& fname, uint32_t numTerms) {
    std::string term = "term";
    term.resize(12);

    writer->startField(fname);
    for (int i=0; i<numTerms; i++) {
      // std::format not implemented yet...
      sprintf(term.data()+4,"%08d",i);
      addTerm(term, getNumDocs(numTerms));
    }
    writer->endField(fname);
  }

  void addFields(uint32_t numFields) {
    std::string fname = "field";
    fname.resize(13);

    for (int i=0; i<numFields; i++) {
      // std::format not implemented yet...
      sprintf(fname.data()+5,"%08d",i);
      addField(fname, getNumTerms(numFields));
    }
  }

};


TEST_F(PostingsTest, basic) {
  RAMDir dir;
  MemPool pool;
  PostingsWriter writer(dir, "gen1");
  writer.startField("field1");
  std::string t1 = "term1";
  TermRef term1(pool,t1.data(),t1.size());
  writer.startTerm(term1);
  writer.startDoc(7);
  writer.addPositionDelta(5);
  writer.addPositionDelta(3);
  writer.addPositionDelta(10);
  writer.endDoc(7);
  writer.endTerm(term1);
  writer.endField("field1");
  writer.finish();


  auto tindexFile = dir.openFile("tindex");
  auto termFile = dir.openFile("term");
  auto docFile = dir.openFile("doc") ;
  auto posFile = dir.openFile("pos");
  PostingsReader reader(tindexFile.get(), termFile.get(), docFile.get(), posFile.get());
  TermIndexReader tindexReader(pool, reader);
  tindexReader.readNextField();
  std::cout << "FIELD NAME name=" << tindexReader.name() << " numTerms=" << tindexReader.numTerms() << std::endl;

  TermsEnum tenum(pool, reader, tindexReader);
  while (tenum.nextTerm()) {
    std::cout << "TERM=" << tenum.term() << " ord=" << tenum.ord() << std::endl;
  }

  DocsEnum docsEnum(pool, reader, tindexReader, tenum);
  auto ndocs = docsEnum.numDocs();
  std::cout << "\tnumDocs=" << docsEnum.numDocs() << " totalTermFreq=" << docsEnum.totalTermFreq() << std::endl;

  for (int i=0; i<ndocs; i++) {
    auto id = docsEnum.nextDoc();
    auto tfreq = docsEnum.termFreq();
    std::cout << "\t\tdocid=" << id << " termFreq=" << tfreq << std::endl;
    docsEnum.startPositions();
    for (int j=0; j<tfreq; j++) {
      auto pos = docsEnum.nextPosition();
      std::cout << "\t\t\tpos=" << pos << std::endl;
    }
  }

}



TEST_F(PostingsTest, randWrite) {
 std::cout << "SEED=" << rng_seed << std::endl;
 init();
  addFields(2);
}
