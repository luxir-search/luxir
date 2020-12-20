
#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"


TEST(PostingsWriter, test_basic) {
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
