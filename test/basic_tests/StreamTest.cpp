
#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/Stream.h"
#include "simdcomp/include/for.h"
#include "SoluxTest.h"

using namespace std;

namespace solux {

class StreamTest : public SoluxTest {
public:

// Write a bunch of random bytes and then ensure we can read them back verbatim.
// A PNRG with a known seed is perfect to test this w/o having to store the whole sequence.
  void testStream(MemPool &pool, int nbytes) {
    Stream stream;

    // capture rng state so we can replay to compare
    auto saved_rng = rng;
    // saved_rng();  // uncomment this, and it should cause a failure because the rngs will not be synchronized.
    for (int i = 0; i < nbytes; i++) {
      stream.writeByte(pool, (uint8_t) rng.rbyte());
    }

    ASSERT_EQ(nbytes, stream.size(pool));

    auto reader = stream.begin(pool);
    auto end = stream.end(pool);
    // StreamReader reader(stream, pool);

    for (int i = 0; i < nbytes; i++) {
      char c;
      ASSERT_TRUE(reader != end);
      if (i & 0x02) { // alternate every few calls
        c = *reader++; // use postfix
      } else {
        c = *reader;
        ++reader;      // use prefix
      }
      ASSERT_EQ((char) saved_rng.rbyte(), c);
    }
    ASSERT_TRUE(reader == end);
  }
};


TEST_F(StreamTest, basic) {
  //
  // Simplest code possible to try and tease out any optimization issue arising over strict aliasing violation (see bbStart_ in Stream).
  // This code should be tested with maximum optimization flags.
  //
  MemPool pool;
  Stream stream;

  stream.writeByte(pool, 1);
  stream.writeByte(pool, 2);
  stream.writeByte(pool, 3);
  stream.writeByte(pool, 4);
  stream.writeByte(pool, 5);

  ASSERT_EQ(5, stream.size(pool));

  StreamReader reader(stream, pool);
  int total = 0;
  total += *reader;
  ++reader;
  total += *reader;
  ++reader;
  total += *reader;
  ++reader;
  total += *reader;
  ++reader;
  total += *reader;

  ASSERT_EQ(1 + 2 + 3 + 4 + 5, total);

  int total2 = 0;
  for (Stream::iterator iter = stream.begin(pool); iter != stream.end(pool); total2 += *iter++);
  ASSERT_EQ(total, total2);

}

TEST_F(StreamTest, randStream) {
  int maxlen = MemPool::BYTE_BLOCK_SIZE * 2;
  int iter = 100;
  int minBytesToWrite = 1000000;

  MemPool pool;
  for (int i = 0; i < iter || pool.size() <= minBytesToWrite; i++) {
    // test many small, but some big.
    int slen = rng.rint(20);
    if (slen == 18) slen = rng.rint(100);
    if (slen == 19) slen = rng.rint(maxlen);
    testStream(pool, slen);

    // TODO: rewind the pool occasionally so we don't use more memory than necessary.
  }
}

} // end namespace