
#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/DocStream.h"
#include "test/SoluxTest.h"

using namespace std;

using namespace solux;


class TestConsumer {
public:
  int64_t dochash = 1;
  int64_t poshash = 1;
  int lastDoc = -1;
  int position = 0;
  bool operator==(TestConsumer const&) const = default;

  void startDoc(int docid) {
    ASSERT_GT(docid , lastDoc);
    lastDoc = docid;
    position = 0;
  }
  void endDoc(int docid) {
    ASSERT_EQ(docid, lastDoc);
    dochash = dochash*29 + lastDoc;
  }
  void addPositionDelta(int delta) {
    position += delta;
    ASSERT_GT(position, -1);  // TODO: can positions be 0?
    poshash = poshash*29 + position;
  }
};


class InverterTest : public SoluxTest {
public:
  // add a doc and some random positions to both docstream and consumer
  // return false if we can't add any more.
  bool addDoc(MemPool& pool, DocFreqPosStream& docstream, TestConsumer& c) {
    int docDelta = rng.rint(1,300);
    if (c.lastDoc + docDelta + 1 < 0) { // add 1 if we are reserving INT_MAX for sentinel
      return false;
    }
    int docid = c.lastDoc + docDelta;
    c.startDoc(docid);
    int nPos = rng.rint(1,100);
    addPositions(pool, docstream, c, nPos);
    c.endDoc(docid);
    return true;
  }

  void addPositions(MemPool& pool, DocFreqPosStream& docstream, TestConsumer& c, int numPositions) {
    int maxDelta = numPositions==0 ? 0 : INT_MAX / numPositions;
    for (int i=0; i<numPositions; i++) {
      auto delta = rng.rint(1,maxDelta);
      c.addPositionDelta(delta);
      docstream.addDoc(pool, c.lastDoc, c.position);
    }
  }

};


TEST_F(InverterTest, simplePos) {
  //
  // Simplest code possible to try and tease out any optimization issue arising over strict aliasing violation (see bbStart_ in Stream).
  // This code should be tested with maximum optimization flags.
  //
  MemPool pool;
  DocFreqPosStream docstream(pool, 3,2);
  TestConsumer c1;
  c1.startDoc(3); c1.addPositionDelta(2); c1.endDoc(3);
  TestConsumer c;
  docstream.pushDocs(pool, c);
  ASSERT_EQ(c1, c);

  DocFreqPosStream ds2(pool, 3,2);
  ds2.addDoc(pool, 5, 6);
  ds2.addDoc(pool, 5, 13);
  c1 = TestConsumer();
  c1.startDoc(3); c1.addPositionDelta(2); c1.endDoc(3);
  c1.startDoc(5); c1.addPositionDelta(6); c1.addPositionDelta(7); c1.endDoc(5);
  c = TestConsumer();
  ds2.pushDocs(pool, c);
  ASSERT_EQ(c1, c);
}



TEST_F(InverterTest, interleavedPos) {
  //
  // Simplest code possible to try and tease out any optimization issue arising over strict aliasing violation (see bbStart_ in Stream).
  // This code should be tested with maximum optimization flags.
  //
  MemPool pool;
  auto save = pool.getSavePoint();

  int iter=100;
  for (int i=0; i<iter; i++) {
    DocFreqPosStream d1(pool, 3, 2);
    TestConsumer c1;
    c1.startDoc(3);
    c1.addPositionDelta(2);
    //addPositions(pool, d1, c1, rng.rint(10)); // sometimes add 0 additional positions to test boundaries.
    c1.endDoc(3);

    DocFreqPosStream d2(pool, 4, 5);
    TestConsumer c2;
    c2.startDoc(4);
    c2.addPositionDelta(5);
    //addPositions(pool, d2, c2, rng.rint(10)); // sometimes add 0 additional positions to test boundaries.
    c2.endDoc(4);

    int nDocs = rng.rint(100);
    for (int dnum = 0; dnum < nDocs; dnum++) {
      if (!addDoc(pool, d1, c1)) break;
      if (!addDoc(pool, d2, c2)) break;
    }

    TestConsumer c1a;
    d1.pushDocs(pool, c1a);
    ASSERT_EQ(c1, c1a);

    TestConsumer c2a;
    d2.pushDocs(pool, c2a);
    ASSERT_EQ(c2, c2a);

    // std::cout << "sizeof pool=" << pool.size() << std::endl;

    // wait to cross a few block boundaries before resetting.
    if (pool.size() > MemPool::BYTE_BLOCK_SIZE*4) {
      pool.rewind(save);
    }

  }
}

