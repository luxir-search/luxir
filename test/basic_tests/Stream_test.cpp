
#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/Stream.h"
#include "simdcomp/include/for.h"
#include "test_util.h"

using namespace std;


// Write a bunch of random bytes and then ensure we can read them back verbatim.
// A PNRG with a known seed is perfect to test this w/o having to store the whole sequence.
void testStream(MemPool& pool, int nbytes, int startVal) {
  Stream stream;
  if (startVal==0) startVal=1;

  uint64_t val = startVal;
  for (int i=0; i<nbytes; i++) {
    val = xorshift(val);
    stream.writeByte(pool, (uint8_t)val);
  }

  ASSERT_EQ(nbytes, stream.size(pool));

  auto reader = stream.begin(pool);
  auto end = stream.end(pool);
  // StreamReader reader(stream, pool);
  val = startVal;
  for (int i=0; i<nbytes; i++) {
    val = xorshift(val);
    char c;
    ASSERT_TRUE(reader != end);
    if (i & 0x02) { // alternate every few calls
      c = *reader++; // use postfix
    } else {
      c = *reader;
      ++reader;      // use prefix
    }
    ASSERT_EQ((char)val, c);
  }
  ASSERT_TRUE(reader == end);
}

/////////////////////////////////////////////////////////////////////////////

TEST(stream_test, test_basic) {


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

  ASSERT_EQ(1+2+3+4+5, total);

  int total2 = 0;
  for (Stream::iterator iter = stream.begin(pool); iter != stream.end(pool); total2 += *iter++);
  ASSERT_EQ(total, total2);

}

TEST(stream_test, rand_stream) {
  int maxlen = MemPool::BYTE_BLOCK_SIZE * 2;
  int iter=1000;
  int minBytesToWrite = 1000000;

  MemPool pool;
  for (int i=0; i<iter || pool.size() <= minBytesToWrite; i++) {
    // test many small, but some big.
    int slen = rint(20);   // TODO: repeatable seeds in random number generators
    if (slen==18) slen = rint(100);
    if (slen==19) slen = rint(maxlen);
    testStream(pool, slen, rint(1000000000));
  }
}

