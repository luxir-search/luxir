
#include <gtest/gtest.h>
#include <iostream>
#include <new>
#include <luxir/util/random.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include "luxir/util/luxir_util.h"
#include "luxir/util/heap.h"
#include "luxir/util/TaggedPtr.h"

using namespace std;

// Basic sanity tests

TEST(BasicTest, testCompiler) {
    // Some of our basic expectations...
    // We would need a thorough code review if any of these are broken.
    EXPECT_EQ(8, sizeof(void*));
    // MSVC has 32 bit longs!  EXPECT_EQ(8, sizeof(long));
    EXPECT_EQ(8, sizeof(int64_t));
    EXPECT_EQ(4, sizeof(int));
    EXPECT_EQ(4, sizeof(unsigned));

#ifdef REMOVED // not implemented in clang yet
    // We don't require <=256, it's just a sanity check (i.e. we haven't seen it happen)
    EXPECT_TRUE(std::hardware_destructive_interference_size > 0 && std::hardware_destructive_interference_size <= 256);
#endif

    LUXIR_UNALIGNED_START
    struct alignas(1) s2 {
      int64_t x;
      char c;
      int32_t z;
      char d;
    } LUXIR_UNALIGNED_END;

    EXPECT_EQ(sizeof(int64_t)+sizeof(char)*2+sizeof(int32_t), sizeof(s2));  // make sure there is no padding

    // tagged pointer on heap
    TaggedPtr<std::string> tp(new std::string("hi"),5);
    EXPECT_EQ(*tp.ptr(), "hi");
    EXPECT_EQ(tp.tag(), 5);
    delete tp.ptr();

    // tagged pointer on stack
    std::string stackString("hi");
    TaggedPtr<std::string> tp2(&stackString,3);
    EXPECT_EQ(*tp2.ptr(), "hi");
    EXPECT_EQ(tp2.tag(), 3);
}


TEST(BasicTest, testPQ) {


  /* alternate form... need to use decltype for lambda
  auto constexpr mycmp = [](double a, double b){return b < a;}; // reversed comparator for a min heap
  luxir::IndirectPQ<float, decltype(mycmp)> pq(vals, valPtrs);
  */

  {
    std::vector<float> vals = {50.0, 75.0, 25.0};
    std::vector<float*> valPtrs;
    valPtrs.resize(vals.size());

    luxir::IndirectPQ<float, std::greater<>> pq(vals, valPtrs);
    ASSERT_EQ(vals.size(), pq.size());
    ASSERT_EQ(pq.top(), 25.0);
    ASSERT_EQ(pq.indexOfTop(), 2);
    pq.top() = 125;
    pq.updateTop();
    ASSERT_EQ(pq.top(), 50.0);
    ASSERT_EQ(pq.indexOfTop(), 0);
    pq.removeTop();
    ASSERT_EQ(pq.top(), 75.0);
    ASSERT_EQ(pq.indexOfTop(), 1);
    pq.removeTop();
    ASSERT_EQ(pq.top(), 125.0);
    ASSERT_EQ(pq.indexOfTop(), 2);
    pq.removeTop();
    ASSERT_EQ(pq.size(), 0);
  }

  {
    std::vector<float> vals = {50.0, 75.0, 25.0, 80.0, 40.0};
    std::vector<float*> valPtrs(3);
    luxir::IndirectPQ<float, std::greater<>> pq(valPtrs, 0);
    ASSERT_EQ(pq.size(), 0);
    ASSERT_EQ(nullptr, pq.insertWithOverflow(&vals[4]));  // 40
    ASSERT_EQ(nullptr, pq.insertWithOverflow(&vals[0]));  // 50
    ASSERT_EQ(pq.top(), 40.0);
    ASSERT_EQ(nullptr, pq.insertWithOverflow(&vals[1]));  // 75
    ASSERT_EQ(pq.top(), 40.0);
    float* ejected = pq.insertWithOverflow(&vals[3]);     // 80, kicks out 40
    ASSERT_EQ(&vals[4], ejected);
    ASSERT_EQ(pq.top(), 50.0);
    ejected = pq.insertWithOverflow(&vals[2]);            // 25, rejected
    ASSERT_EQ(&vals[2], ejected);
    ASSERT_EQ(pq.top(), 50.0);

    valPtrs[0] = &vals[4];  // 40
    valPtrs[1] = &vals[2];  // 25
    luxir::IndirectPQ<float, std::greater<>> pq2(vals, valPtrs, size_t(2));
    ASSERT_EQ(pq2.size(), 2);
    ASSERT_EQ(pq2.top(), 25.0f);
    ASSERT_EQ(pq2.indexOfTop(), 2);
  }

  {
    std::vector<float> vals = {50.0, 75.0, 25.0, 80.0, 40.0};
    std::vector<int> valPtrs(3);
    luxir::IndexedPQ<float, std::greater<>> pq(vals, valPtrs, 0);
    ASSERT_EQ(pq.size(), 0);
    ASSERT_EQ(false, pq.insertWithOverflow(40.0f));  // 40
    ASSERT_EQ(false, pq.insertWithOverflow(50.0f));  // 50
    ASSERT_EQ(pq.top(), 40.0);
    ASSERT_EQ(false, pq.insertWithOverflow(75.0f));  // 75
    ASSERT_EQ(pq.top(), 40.0);
    bool ejected = pq.insertWithOverflow(80.0f);     // 80, kicks out 40
    ASSERT_EQ(ejected, true);
    ASSERT_EQ(pq.top(), 50.0);
    ejected = pq.insertWithOverflow(25);            // 25, rejected
    ASSERT_EQ(ejected, false);
    ASSERT_EQ(pq.top(), 50.0);
  }


  {
    int n = 100;
    luxir::Rng rng;
    std::vector<int> vals(n);
    for (auto& val : vals) {
      val = rng.rint(1000000);
    }
    std::vector<int> sorted = vals;
    std::ranges::sort(sorted);

    std::vector<int*> ptrs(vals.size());
    luxir::IndirectPQ<int, std::greater<>> pq(vals, ptrs);

    std::vector<int> indexes(vals.size());
    luxir::IndexedPQ<int, std::greater<>> indexedPQ(vals, indexes);

    ASSERT_EQ(pq.top(), sorted[0]);

    for (auto expected : sorted) {
      ASSERT_EQ(pq.top(), expected);
      ASSERT_EQ(indexedPQ.top(), expected);
      auto idx = pq.indexOfTop();
      ASSERT_EQ(vals[idx], expected);
      ASSERT_EQ(idx, indexedPQ.indexOfTop());
      pq.removeTop();
      indexedPQ.removeTop();
    }
  }
}

TEST(BasicTest, testMap) {
  // test boost's unordered_flat_map behavior of iteration and erase().
  // some notes said that erase() was non-standard (i.e. didn't return the next item), but it seems to work fine.
  {
    boost::unordered_flat_map<uint64_t, uint64_t> map;
    map[1] = 1;
    map[7] = 7;
    map[100] = 100;
    map[5] = 5;
    map[20] = 20;
    map[25] = 25;
    map[17] = 17;
    map[15] = 15;

    for (auto iter = map.begin(); iter != map.end(); ) {
      if (iter->first & 0x01) { // delete odd entries
        iter = map.erase(iter);
      } else {
        iter++;
      }
    }

    ASSERT_EQ(2, map.size());
    // verify that the remaining entries are even
    for (auto iter = map.begin(); iter != map.end(); iter++) {
      ASSERT_EQ(0, iter->first & 0x01);
    }
  }
}

#if REMOVED_CODE
char* returnsStackAddr(char* ptr) {
  char onstack[10];
  onstack[0]='A';
  auto addr = reinterpret_cast<uint64_t>(onstack);
  addr += xorshift(0) + xorshift(reinterpret_cast<uint64_t>(ptr) % 2);
  if (ptr != nullptr) {
    ptr = reinterpret_cast<char*>(addr);
  }
  return ptr;
}

// TODO: get memory sanitizer working (requires everything linked to be compiled with that!)
// If you want to try out various sanitizers, uncomment one of the BUG lines below.
TEST(BasicTest, testAddressSanitizer) {
  constexpr int size = 16;
  char onstack[size];
  char* arr = onstack;
  char* ptr = (char*)malloc(size);
  memset(ptr,'A',size);
  free(ptr);
  ptr = (char*)malloc(size);
  char x = 0;
  uint64_t off = xorshift(0) + xorshift(reinterpret_cast<uint64_t>(ptr) % 2);  // try to foil static analysis (this should be 0 though)
  x = ptr[0 + off]; // BUG: uninitialized memory read... requires memory sanitizer, not address sanitizer (or valgrind)
  // if (x==55) { off = 0; }  // BUG: valgrind correctly detects the first conditional use of uninitialized memory.
  memset(ptr+off,0+off,10+off); // ok, initialize buffer
  // ptr[10+off] = x; // BUG: one past end write
  // arr[10+off] = x; // BUG: one past end write on stack

  // memcpy(ptr+1+off, arr+off, size+off); // BUG: write past end with memcpy
  // memcpy(arr+1+off, ptr+off, size+off); // BUG: write past end on stack with memcpy
  // memcpy(ptr-1+off, arr+off, size+off); // BUG: write before beginning with memcpy
  // memcpy(arr-1+off, ptr+off, size+off); // BUG: write before beginning on stack with memcpy

  // if (ptr[0]=='Z' || arr[0]=='Z') {cout<<"How?";}  // BUG. use the arrays to try and prevent optimizing away. clang didn't detect last underflow w/o this uncommented.

  char* ptr2 = returnsStackAddr(ptr);
  // if (*ptr2 == 'A') { cout<<"Oops, stack frame no longer exists!"<<endl; }  // BUG: use old stack frame
  // NOTE: clion set detect_stack_use_after_return=false as default for some reason.  Change it to true, and
  // both clang and g++ catch this bug.


  free(ptr); // comment out for leak test
  // free(ptr); // BUG: double free
  ***/
}
#endif


// test too large tokens
// test too large positions
// test position overflow
// test weird first position (lucene disallows 0)
// test that indexing exception (too large token, etc) doesn't mess up anything because of partial index (what are our invariants?)
//  things like norms we may not get to?
