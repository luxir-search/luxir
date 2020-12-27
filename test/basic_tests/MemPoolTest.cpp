#include "SoluxTest.h"
#include "solux/util/MemPool.h"

using namespace std;

class MemPoolTest : public SoluxTest {
protected:
};


TEST_F(MemPoolTest, rewind) {
  MemPool pool;
  pool.allocate(3);
  char* a = pool.allocate(2);
  auto savePoint = pool.getSavePoint();
  char* b = pool.allocate(3);
  *b = 'b';
  char* c = pool.allocate(4);
  *c = 'c';
  pool.rewind(savePoint);
  char* bb = pool.allocate(3);
  ASSERT_EQ(b, bb);
  assert(*bb != 'b');  // in debug mode, we should have stomped on the memory
#ifdef NDEBUG
  // if we aren't in debug mode, our memory should be untouched (TODO: unless we instructed the pool to use malloc!)
  ASSERT_EQ('b', *bb);
#endif
}

// test page boundary conditions efficiently using rewind
TEST_F(MemPoolTest, boundary) {
  MemPool pool;
  size_t sz = MemPool::BYTE_BLOCK_SIZE;
  char* a = pool.allocate(1);
  auto savePoint = pool.getSavePoint();
  char* p = pool.ptr();
  char* b = pool.allocate(sz - 1);  // should be room for this.
  ASSERT_EQ(p, b);
  pool.rewind(savePoint);
  savePoint = pool.getSavePoint();
  ASSERT_EQ(p, pool.ptr());
  char* bb = pool.allocate(sz);  // should not be room for this.
  ASSERT_NE(p, bb);
  *bb = 'b';

  pool.rewind(savePoint);

  char* mem = new char[sz];  // if we freed the last block, try to foil malloc from returning the same one to the pool again
  mem[0] = 'A'; // try to avoid the malloc being optimized away

  ASSERT_EQ(p, pool.ptr());
  char* bbb = pool.allocate(sz);  // should not be room for this, but when we allocate new space, we should have remembered previous buffer!
  ASSERT_EQ(bb,bbb);
#ifdef NDEBUG
  // check if we got the same buffer in non-debug mode and that the data wasn't touched
  ASSERT_EQ('b', *bbb);
#else
  // check if we got the same buffer in debug mode and that it was scribbled on
  ASSERT_EQ(MemPool::SCRIBBLE_CHAR, *bbb);
#endif

  ASSERT_EQ(mem[0],'A');
  delete[] mem;
}