#include "test/SoluxTest.h"
#include "solux/util/MemPool.h"

using namespace std;
using namespace solux;

class MemPoolTest : public solux::SoluxTest {
protected:
};


TEST_F(MemPoolTest, rewind) {
  MemPool pool;
  pool.allocate(3);
  char* a = pool.allocate(2);
  unused(a);
  auto savePoint = pool.getSavePoint();
  auto sz = pool.size();
  char* b = pool.allocate(3);
  *b = 'b';
  char* c = pool.allocate(4);
  *c = 'c';
  pool.rewind(savePoint);
  // check that the size matches again
  ASSERT_EQ(sz, pool.size());
#ifndef MEMPOOL_MALLOC
  char* bb = pool.allocate(3);
  ASSERT_EQ(b, bb);
  assert(*bb != 'b');  // in debug mode, we should have stomped on the memory
#ifdef NDEBUG
  // if we aren't in debug mode, our memory should be untouched
  ASSERT_EQ('b', *bb);
#endif
#else
  // not much to check if we are using malloc
#endif
}

TEST_F(MemPoolTest, randRewind) {
  MemPool pool;

  int maxStates = 100;
  vector<std::pair<MemPool::save_point, size_t>> states;
  for (int i=0; i<1000; i++) {
    if (rng.rbool() && (int)states.size() < maxStates) {
      states.emplace_back(pool.getSavePoint(), pool.size());
    } else if (rng.rbool() && states.size() > 0) {
      auto [save,sz] = states.back();
      states.pop_back();
      int nBuffersToSave=rng.rint(4);
      pool.rewind(save, nBuffersToSave);
      ASSERT_EQ(sz, pool.size());
    }
    int nallocs = rng.rint(6);
    for (int i=0; i<nallocs; i++) {
      int allocSz = rng.rint(1, MemPool::BYTE_BLOCK_SIZE);
      char* x = pool.allocate(allocSz);
      x[0] = 'A';  // touch beginning and end
      x[allocSz-1] = 'A';
    }
  }
}

#ifndef MEMPOOL_MALLOC
// test page boundary conditions efficiently using rewind
TEST_F(MemPoolTest, boundary) {
  MemPool pool;
  size_t sz = MemPool::BYTE_BLOCK_SIZE;
  char* a = pool.allocate(1);
  unused(a);
  auto savePoint = pool.getSavePoint();
  auto poolSz = pool.size();
  char* p = pool.ptr();
  char* b = pool.allocate(sz - 1);  // should be room for this.
  ASSERT_EQ(p, b);
  pool.rewind(savePoint);
  ASSERT_EQ(poolSz, pool.size());
  savePoint = pool.getSavePoint();
  ASSERT_EQ(p, pool.ptr());
  char* bb = pool.allocate(sz);  // should not be room for this.
  ASSERT_NE(p, bb);
  *bb = 'b';

  pool.rewind(savePoint);
  ASSERT_EQ(poolSz, pool.size());

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
#endif