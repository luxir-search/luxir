// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <gtest/gtest.h>
#include <iostream>
#include <luxir/index/Inverter.h>

#include "luxir/index/DocStream.h"
#include "test/LuxirTest.h"

using namespace std;

using namespace luxir;


class TestConsumer {
public:
  uint64_t dochash = 1;
  uint64_t poshash = 1;
  int lastDoc = -1;
  int position = -1;
  bool operator==(TestConsumer const&) const = default;

  void startDoc(int docid) {
    // std::cout << docid << "," << std::flush;
    ASSERT_GT(docid , lastDoc);
    dochash = dochash*29 + lastDoc;
    lastDoc = docid;
    position = -1;
  }
  void endDoc(int docid) {
    ASSERT_EQ(docid, lastDoc);
    dochash = dochash*29 + lastDoc;
  }
  void addPositionDelta(int delta) {
    // std::cout << "\t\tdelta=" << delta << " pos=" << position << " pos+delta=" << position+delta << std::endl;
    position += delta;
    ASSERT_GT(position, -1);  // TODO: can positions be 0?
    poshash = poshash*29 + position;
  }

  void addInt32(int32_t val) {
    dochash = dochash*31 + val;
  }
  void addInt64(int64_t val) {
    dochash = dochash*33 + val;
  }
};


class InverterTest : public LuxirTest {
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
  DocFreqPosStream docstream(pool, 3,1);
  TestConsumer c1;
  c1.startDoc(3); c1.addPositionDelta(2); c1.endDoc(3);
  TestConsumer c;
  docstream.pushDocs(pool, c);
  ASSERT_EQ(c1, c);

  DocFreqPosStream ds2(pool, 3,1);
  ds2.addDoc(pool, 5, 6);
  ds2.addDoc(pool, 5, 13);
  c1 = TestConsumer();
  c1.startDoc(3); c1.addPositionDelta(2); c1.endDoc(3);
  c1.startDoc(5); c1.addPositionDelta(7); c1.addPositionDelta(7); c1.endDoc(5);
  c = TestConsumer();
  ds2.pushDocs(pool, c);
  ASSERT_EQ(c1, c);
}

TEST_F(InverterTest, duplicateTermPositionsCanonicalizedAndDecreasingRejected) {
  MemPool pool;
  DocFreqPosStream stream(pool, 0, 0);
  stream.addDoc(pool, 0, 0);
  stream.addDoc(pool, 0, 2);
  EXPECT_THROW(stream.addDoc(pool, 0, 1), std::runtime_error);

  TestConsumer actual;
  stream.pushDocs(pool, actual);
  TestConsumer expected;
  expected.startDoc(0);
  expected.addPositionDelta(1);
  expected.addPositionDelta(2);
  expected.endDoc(0);
  EXPECT_EQ(expected, actual);
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
    DocFreqPosStream d1(pool, 3, 1);
    TestConsumer c1;
    c1.startDoc(3);
    c1.addPositionDelta(2);
    //addPositions(pool, d1, c1, rng.rint(10)); // sometimes add 0 additional positions to test boundaries.
    c1.endDoc(3);

    DocFreqPosStream d2(pool, 4, 4);
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


TEST_F(InverterTest, docs) {
  MemPool pool;

  /* for testing specific scenarios by hand
  {
    DocStream docs(pool, 0);
    docs.addDoc(pool, 1000);
    TestConsumer c2;
    docs.pushDocs(pool, c2);
  }
  */

  int poolSize = pool.size();

  // test big run starting at 0
  DocStream docs(pool);
  TestConsumer c;
  int ndocs = rng.rint(200);
  for (int i=0; i<ndocs; i++) {
    docs.addDoc(pool, i);
    c.startDoc(i);
  }

  int poolUsage = pool.size() - poolSize;
  ASSERT_LT(poolUsage, 8); // ensure minimal memory usage for one big run

  TestConsumer c1;
  docs.pushDocs(pool, c1);
  ASSERT_EQ(c1, c);


  int iter = 100;
  // maximum number of regions per set... each region has a separate maxgap between docs to better test compressed
  // bitset implementations.
  int maxregions = 200;
  int maxdocperregion = 1000;

  auto savepoint= pool.getSavePoint();
  for (int i=0; i<iter; i++) {
    pool.rewind(savepoint);
    TestConsumer consumer1;
    // std::cout << std::endl << "CREATING" << std::endl;
    int docid = rng.rbool() ? rng.rint(10) : rng.rint(65536*10);  // starting doc... very small or all over the place
    DocStream docs(pool);
    docs.addDoc(pool, docid);
    consumer1.startDoc(docid);

    int regions = rng.rint(maxregions);
    for (int j=0; j<regions; j++) {
      int maxgap = rng.rbool() ? rng.rint(1,20) : rng.rint(1,65536*2);
      // number of docs in this region... lower if maxgap is high to not overrun maxint
      int ndocs = maxgap < 1000 ? rng.rint(1,maxdocperregion) : rng.rint(1,20);
      ndocs = 1;
      for (int k=0; k<ndocs; k++) {
        docid += rng.rint(1,maxgap+1);
        if (docid < 0) break;  // overflow
        docs.addDoc(pool, docid);
        consumer1.startDoc(docid);
      }
      if (docid < 0) break;  // overflow
    }

    // std::cout << std::endl << "READING" << std::endl;
    TestConsumer consumer2;
    docs.pushDocs(pool, consumer2);
    ASSERT_EQ(consumer1, consumer2);
  }

}


TEST_F(InverterTest, intstream) {
  MemPool pool;

  int iter = 100;
  int maxvals = 1000;

  auto savepoint = pool.getSavePoint();
  for (int i = 0; i < iter; i++) {
    pool.rewind(savepoint);
    IntStream ints(pool);
    TestConsumer consumer1;

    int nvals = rng.rint(1, maxvals);
    int lastval = 0;
    for (int j=0; j<nvals; j++) {
      int val;
      if (rng.rbool()) {
        val = lastval + rng.rint(-256,256);
        lastval = val;
      } else {
        val = (int32_t)rng();
      }
      ints.addVal(pool, val);
      consumer1.addInt32(val);
    }

    TestConsumer consumer2;
    ints.pushValues(pool, consumer2);

    ASSERT_EQ(consumer1, consumer2);
  }
}

TEST_F(InverterTest, longstream) {
  MemPool pool;

  int iter = 1;
  int maxvals = 10;

  auto savepoint = pool.getSavePoint();
  for (int i = 0; i < iter; i++) {
    pool.rewind(savepoint);
    LongStream longs(pool);
    TestConsumer consumer1;

    int nvals = rng.rint(1, maxvals);
    int64_t lastval = 0;
    for (int j=0; j<nvals; j++) {
      int64_t val;
      if (rng.rbool()) {
        val = lastval + rng.rint(-256,256);
        lastval = val;
      } else {
        val = (int64_t)rng();
      }
      val = -1000000+j;
      longs.addVal(pool, val);
      consumer1.addInt64(val);
    }

    TestConsumer consumer2;
    longs.pushValues(pool, consumer2);

    ASSERT_EQ(consumer1, consumer2);
  }
}
