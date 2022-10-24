#include "test/SoluxTest.h"
#include "solux/util/MemPool.h"
#include "gtl/phmap.hpp"

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


// test allocation of objects
TEST_F(MemPoolTest, alloc) {
  MemPool pool;
  int cons_calls = 0;
  int des_calls = 0;
  auto start_size = pool.size();
  {
    auto x = pool.make_unique<X>(cons_calls, des_calls);
    auto y = pool.make_unique<X>(cons_calls, des_calls);
    auto z = pool.make_unique<X>(cons_calls, des_calls);
    y->nested = std::move(z);
    ASSERT_TRUE(z == nullptr);
    ASSERT_EQ(cons_calls, 3);
    ASSERT_EQ(des_calls, 0);

#ifndef MEMPOOL_MALLOC
    ASSERT_EQ(pool.size()-start_size, 3*sizeof(X));
#endif
  }
  ASSERT_EQ(cons_calls, 3);
  ASSERT_EQ(des_calls, 3);

  start_size = pool.size();
  int start_cons_calls = cons_calls;

  // now try custom allocator
  {
    std::map<int, X, std::less<>, MemPool::allocator<std::pair<const int, X>>> map(pool.getAllocator());
    map.try_emplace(1, cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+1);
    map.try_emplace(2, cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+2);
    map.try_emplace(3, cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+3);

#ifndef MEMPOOL_MALLOC
    ASSERT_TRUE(size_t(pool.size() - start_size) > sizeof(*map.begin())*map.size());
#endif
  }
  ASSERT_EQ(cons_calls, des_calls);

  start_size = pool.size();
  start_cons_calls = cons_calls;

  // now try MemPool with polymorphic allocator with standard vector
  {
    pmr::polymorphic_allocator<X> pmr_alloc(&pool);
    std::vector<X, std::pmr::polymorphic_allocator<X>> myvec(pmr_alloc);
    myvec.reserve(4);
    myvec.emplace_back(cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+1);
    myvec.emplace_back(cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+2);
    ASSERT_TRUE(size_t(pool.size() - start_size) > sizeof(X)*myvec.size());
  }
  ASSERT_EQ(cons_calls, des_calls);

  start_size = pool.size();
  start_cons_calls = cons_calls;

  // now try mempool as a memory_resource with std::pmr::vector
  {
    std::pmr::vector<X> myvec(&pool);
    myvec.reserve(4);
    myvec.emplace_back(cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+1);
    myvec.emplace_back(cons_calls, des_calls);
    ASSERT_EQ(cons_calls, start_cons_calls+2);
    ASSERT_TRUE(size_t(pool.size() - start_size) > sizeof(X)*myvec.size());
  }
  ASSERT_EQ(cons_calls, des_calls);

  start_size = pool.size();
  start_cons_calls = cons_calls;

  {
    // create unordered map with nested vectors of unique pointers, all in the mempool
    using keytype = std::string_view;
    using elemtype = u_ptr<X>;
    using valtype = std::vector<elemtype, MemPool::allocator<elemtype>>;
    using pairtype = std::pair<const keytype, valtype>;

#ifndef MEMPOOL_MALLOC
    auto poolPtr = pool.ptr();
#endif

    using Map = gtl::node_hash_map<keytype, valtype, std::hash<std::string_view>, std::equal_to<>, MemPool::allocator<pairtype>>;
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

#ifndef MEMPOOL_MALLOC
    ASSERT_TRUE(size_t(pool.size() - start_size) > sizeof(pairtype)*map.size() + sizeof(elemtype)*10 + sizeof(X)*2);

    // Try to see that everything was actually pool allocated.  Only works if pool hasn't switched pages.
    auto endPoolPtr = pool.ptr();
    void* ptr;
    ptr = &*map.begin();
    ASSERT_TRUE(ptr >= poolPtr && ptr < endPoolPtr);
    pairtype& pair = *map.find("hi");
    ptr = &pair;
    ASSERT_TRUE(ptr >= poolPtr && ptr < endPoolPtr);
    ptr = &pair.second[0];  // the array the vector points to
    ASSERT_TRUE(ptr >= poolPtr && ptr < endPoolPtr);
    ptr = pair.second[3].get();  // one of the X objects we previously created
    ASSERT_TRUE(ptr >= poolPtr && ptr < endPoolPtr);
#endif

    ASSERT_EQ(cons_calls, start_cons_calls+2);
    ASSERT_EQ(des_calls, start_cons_calls);
  }
  ASSERT_EQ(cons_calls, des_calls);

}

TEST_F(MemPoolTest, rewind) {
  MemPool pool;
  ASSERT_EQ(pool.size(), 0);
  pool.alloc(3);
  ASSERT_EQ(pool.size(), 3);
  char* a = pool.alloc(2);
  unused(a);
  auto savePoint = pool.getSavePoint();
  auto sz = pool.size();
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
      char* x = pool.alloc(allocSz);
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