#include <boost/sort/spreadsort/string_sort.hpp>

#include "test/SoluxTest.h"
#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"
#include <boost/unordered/unordered_node_map.hpp>

using namespace std;
using namespace solux;

class MemPoolTest : public solux::SoluxTest {
protected:
};

class X {
public:
  int& cons_calls;
  int& des_calls;
  solux::u_ptr<X> nested;

  X(int& cons_calls, int& des_calls) : cons_calls(cons_calls), des_calls(des_calls) {
    // std::cout << "X::X() this=" << (void*)this << std::endl;
    this->cons_calls++;
  }
  /*  If you use emplace() rather than try_emplace() and call with emplace(1,X(2,3))
   *  then you will need a copy or move constructor to work correctly.
   */


  // This seems to be needed, even though it's not actually used.
  X(const X& other) : cons_calls(other.cons_calls), des_calls(other.des_calls) {
    // std::cout << "X COPY CONSTRUCTOR CALLED! this=" << (void*)this << " otper=" << (void*)&other << std::endl;
    this->cons_calls++;
  }


  /*
  X(X&& other) : cons_calls(other.cons_calls), des_calls(other.des_calls) {
    std::cout << "X MOVE CONSTRUCTOR CALLED! this=" << (void*)this << " otper=" << (void*)&other << std::endl;
  }
  */

  ~X() {
    // std::cout << "X::DESTRUCTOR this=" << (void*)this << std::endl;
    des_calls++;
  }
};


// Object-lifecycle / functional test for MemPool with allocator-aware containers:
// objects construct and destruct correctly and each container operates against
// the pool allocator. Heap-avoidance (that none of this touches the heap) is
// proved directly in MemPoolTest.poolAllocatorsAvoidHeap below.
TEST_F(MemPoolTest, alloc) {
  MemPool pool;
  int cons_calls = 0;
  int des_calls = 0;
  {
    auto x = pool.make_unique<X>(cons_calls, des_calls);
    auto y = pool.make_unique<X>(cons_calls, des_calls);
    auto z = pool.make_unique<X>(cons_calls, des_calls);
    y->nested = std::move(z);
    ASSERT_TRUE(z == nullptr);
    ASSERT_EQ(cons_calls, 3);
    ASSERT_EQ(des_calls, 0);
  }
  ASSERT_EQ(cons_calls, 3);
  ASSERT_EQ(des_calls, 3);

  int start_cons_calls = cons_calls;

  // std::map with MemPool::allocator
  {
    std::map<int, X, std::less<>, MemPool::allocator<std::pair<const int, X>>> map(pool.getAllocator());
    map.try_emplace(1, cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+1);
    map.try_emplace(2, cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+2);
    map.try_emplace(3, cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+3);
  }
  ASSERT_EQ(cons_calls, des_calls);

  start_cons_calls = cons_calls;

  // MemPool via std::pmr::polymorphic_allocator with std::vector
  {
    pmr::polymorphic_allocator<X> pmr_alloc(&pool);
    std::vector<X, std::pmr::polymorphic_allocator<X>> myvec(pmr_alloc);
    myvec.reserve(4);
    myvec.emplace_back(cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+1);
    myvec.emplace_back(cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+2);
  }
  ASSERT_EQ(cons_calls, des_calls);

  start_cons_calls = cons_calls;

  // MemPool as a std::pmr::memory_resource with std::pmr::vector
  {
    std::pmr::vector<X> myvec(&pool);
    myvec.reserve(4);
    myvec.emplace_back(cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+1);
    myvec.emplace_back(cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+2);
  }
  ASSERT_EQ(cons_calls, des_calls);

  start_cons_calls = cons_calls;

  {
    // unordered map with nested vectors of unique pointers, all pool-allocated;
    // exercises the re-emplace-existing-key path and balanced construct/destruct.
    using keytype = std::string_view;
    using elemtype = u_ptr<X>;
    using valtype = std::vector<elemtype, MemPool::allocator<elemtype>>;
    using pairtype = std::pair<const keytype, valtype>;
    using Map = boost::unordered_node_map<keytype, valtype, PackedTermHash, PackedTermEqual, MemPool::allocator<pairtype>>;
    Map map(pool.getAllocator());
    {
      valtype v1(pool.getAllocator());
      valtype &vec = map.try_emplace("hi", std::move(v1)).first->second;
      if (vec.size() == 0) {
        vec.resize(10);
      }
      vec[3] = pool.make_unique<X>(cons_calls, des_calls);
    }
    {
      valtype v1(pool.getAllocator());
      valtype &vec = map.try_emplace("hi", std::move(v1)).first->second;
      if (vec.size() == 0) {
        vec.resize(10);
      }
      vec[5] = pool.make_unique<X>(cons_calls, des_calls);
    }
    ASSERT_EQ(cons_calls, start_cons_calls+2);
    ASSERT_EQ(des_calls, start_cons_calls);
  }
  ASSERT_EQ(cons_calls, des_calls);
}

#ifndef MEMPOOL_MALLOC
// Directly prove MemPool-backed allocators send every allocation into the pool
// and none to the heap, via the allocation counter (memtrack::AllocScope) - the
// strong version of the address-range heuristic in MemPoolTest.alloc above, and
// of the malloc-override-in-a-throwaway-program approach. (Skipped under
// MEMPOOL_MALLOC, where the pool deliberately uses a separate allocation per
// request, so "zero heap" does not apply.)
//
// Subtlety: the pool's backing buffers come from `new char[]`, so growing the
// pool is itself a heap allocation. Each case reserves headroom first (alloc then
// shrink leaves a large current buffer), OUTSIDE the measured scope, so the
// counter sees only the container's own allocations - which must be zero.
TEST_F(MemPoolTest, poolAllocatorsAvoidHeap) {
  int cc = 0, dc = 0;
  auto reserve = [](MemPool& pool, size_t n) { pool.alloc(n); pool.shrink(n); };

  // 1. std::map: red-black-tree nodes route to the pool.
  {
    MemPool pool;
    reserve(pool, 16384);
    memtrack::AllocScope s;
    std::map<int, X, std::less<>, MemPool::allocator<std::pair<const int, X>>> m(pool.getAllocator());
    m.try_emplace(1, cc, dc);
    m.try_emplace(2, cc, dc);
    m.try_emplace(3, cc, dc);
    long heap = s.count();  // capture before EXPECT (gtest allocates) and before ~m
    EXPECT_EQ(0, heap) << "std::map<MemPool::allocator> made " << heap << " heap allocations";
  }

  // 2. std::pmr::vector with the pool as a memory_resource.
  {
    MemPool pool;
    reserve(pool, 16384);
    memtrack::AllocScope s;
    std::pmr::vector<X> v(&pool);
    v.reserve(8);
    v.emplace_back(cc, dc);
    v.emplace_back(cc, dc);
    long heap = s.count();
    EXPECT_EQ(0, heap) << "std::pmr::vector<MemPool> made " << heap << " heap allocations";
  }

  // 3. boost::unordered_node_map of nested pool-vectors of pool unique_ptrs - the
  //    real PackedTermHash shape. Buckets, nodes, inner vectors and the X objects
  //    must all land in the pool.
  {
    MemPool pool;
    reserve(pool, 16384);
    using valtype = std::vector<u_ptr<X>, MemPool::allocator<u_ptr<X>>>;
    using pairtype = std::pair<const std::string_view, valtype>;
    using Map = boost::unordered_node_map<std::string_view, valtype, PackedTermHash, PackedTermEqual,
                                          MemPool::allocator<pairtype>>;
    memtrack::AllocScope s;
    Map map(pool.getAllocator());
    valtype v1(pool.getAllocator());
    valtype& vec = map.try_emplace("hi", std::move(v1)).first->second;
    vec.resize(10);
    vec[3] = pool.make_unique<X>(cc, dc);
    long heap = s.count();
    EXPECT_EQ(0, heap) << "boost::unordered_node_map<MemPool::allocator> made " << heap << " heap allocations";
  }

  // Contrast: the same map with the default allocator DOES hit the heap, so the
  // zeros above are a real result and not a broken-counter false pass.
  {
    memtrack::AllocScope s;
    std::map<int, X, std::less<>> m;
    m.try_emplace(1, cc, dc);
    m.try_emplace(2, cc, dc);
    long heap = s.count();
    EXPECT_GT(heap, 0) << "default-allocator map made no heap allocations (counter broken?)";
  }
}
#endif  // !MEMPOOL_MALLOC

TEST_F(MemPoolTest, sizes) {
#ifndef MEMPOOL_MALLOC
  MemPool pool;
  auto savePoint = pool.getSavePoint();
  ASSERT_EQ(pool.allocatedSize(), MemPool::STATIC_BUFFER_SIZE);
  char* a = pool.alloc(MemPool::STATIC_BUFFER_SIZE + 1);  // should allocate a new buffer
  unused(a);
  ASSERT_EQ(pool.allocatedSize(), MemPool::STATIC_BUFFER_SIZE * 3);  // doubling strategy + original size
  pool.rewind(savePoint);
  ASSERT_EQ(pool.allocatedSize(), MemPool::STATIC_BUFFER_SIZE);
  char* b = pool.alloc(1025);  // should *change* the buffer it had reserved.
  unused(b);
  ASSERT_EQ(pool.allocatedSize(), MemPool::STATIC_BUFFER_SIZE + 2048);
  char* c = pool.alloc(16500);  // should allocate a new buffer of 32K
  unused(c);
  ASSERT_EQ(pool.allocatedSize(), MemPool::STATIC_BUFFER_SIZE + 2048 + 32768);
#endif
}

TEST_F(MemPoolTest, rewind) {
  MemPool pool;
  auto sz = pool.size();
  pool.alloc(3);
  ASSERT_EQ(pool.size(), sz+3);
  char* a = pool.alloc(2);
  unused(a);
  auto savePoint = pool.getSavePoint();
  sz = pool.size();
  char* b = pool.alloc(3);
  *b = 'b';
  char* c = pool.alloc(4);
  *c = 'c';
  pool.rewind(savePoint);
  // check that the size matches again
  ASSERT_EQ(sz, pool.size());



#ifndef MEMPOOL_MALLOC
  char* bb = pool.alloc(3);
  ASSERT_EQ(b, bb);
  assert(*bb != 'b');  // in debug mode, we should have stomped on the memory
#ifdef NDEBUG
  // if we aren't in debug mode, our memory should be untouched
  ASSERT_EQ('b', *bb);
#endif
#else
  // not much to check if we are using malloc
#endif


  // now a scope guard
  sz = pool.size();
  {
    auto guard = pool.rewindScopeGuard();
    char* x = pool.alloc(3);
    *x = 'x';
    char* y = pool.alloc(4);
    *y = 'y';
    ASSERT_GT(pool.size(), sz);
  }
  ASSERT_EQ(pool.size(), sz);

  // thread local rewind
  {
    auto outer = MemPool::threadLocalPoolGuard();
    sz = outer.pool().size();
    {
      auto poolGuard = MemPool::threadLocalPoolGuard();
      ASSERT_EQ(&poolGuard.pool(), &outer.pool());  // same thread, should be same pool
      char* x = poolGuard.pool().alloc(3);
      *x = 'x';
      auto sz2 = poolGuard.pool().size();
      {
        // make sure nested is fine.
        auto poolGuard2 = MemPool::threadLocalPoolGuard();
        ASSERT_EQ(&poolGuard2.pool(), &outer.pool());  // same thread, should be same pool
        char* y = poolGuard2.pool().alloc(5);
        *y = 'y';
      }
      ASSERT_EQ(poolGuard.pool().size(), sz2);
      char* y = poolGuard.pool().alloc(4);
      *y = 'y';
      ASSERT_GT(poolGuard.pool().size(), sz);
    }
    ASSERT_EQ(outer.pool().size(), sz);
  }

  // MemPool::threadLocal().alloc(77);  // this should cause test runner (SoluxTestListener::OnTestEnd) to fail the test
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
      int allocSz = rng.rint(1u, MemPool::BYTE_BLOCK_SIZE - MemPool::HEADER_SIZE);
      char* x = pool.alloc(allocSz);
      x[0] = 'A';  // touch beginning and end
      x[allocSz-1] = 'B';
    }
  }
}

TEST_F(MemPoolTest, align) {
  MemPool pool;

  char* a = pool.alloc(1);
  unused(a);
  MemPool::allocator<size_t> myalloc(pool);
  auto* p = myalloc.allocate(8);
  ASSERT_EQ((size_t)p % 8, 0);

  // test alignment after initial failure to allocate from same block
  p = myalloc.allocate(MemPool::STATIC_BUFFER_SIZE);
  ASSERT_EQ((size_t)p % 8, 0);

}

#ifndef MEMPOOL_MALLOC
// test page boundary conditions efficiently using rewind
TEST_F(MemPoolTest, boundary) {
  MemPool pool;
  size_t sz = MemPool::STATIC_BUFFER_SIZE - MemPool::HEADER_SIZE;  // size of a single block minus the header
  char* a = pool.alloc(1);
  unused(a);
  auto savePoint = pool.getSavePoint();
  auto poolSz = pool.size();
  char* p = pool.ptr();
  char* b = pool.alloc(sz - 1);  // should be room for this.
  ASSERT_EQ(p, b);
  pool.rewind(savePoint);
  ASSERT_EQ(poolSz, pool.size());
  savePoint = pool.getSavePoint();
  ASSERT_EQ(p, pool.ptr());
  char* bb = pool.alloc(sz);  // should not be room for this.
  ASSERT_NE(p, bb);
  *bb = 'b';

  pool.rewind(savePoint);
  ASSERT_EQ(poolSz, pool.size());

  char* mem = new char[sz];  // if we freed the last block, try to foil malloc from returning the same one to the pool again
  mem[0] = 'A'; // try to avoid the malloc being optimized away

  ASSERT_EQ(p, pool.ptr());
  char* bbb = pool.alloc(sz);  // should not be room for this, but when we allocate new space, we should have remembered previous buffer!
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