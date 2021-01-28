
#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/DocStream.h"
#include "test/SoluxTest.h"

using namespace std;

using namespace solux;

class InverterTest : public SoluxTest {
public:
};

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

  // TODO: test interleaved docstreams
}

