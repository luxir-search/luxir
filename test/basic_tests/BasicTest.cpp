
#include <gtest/gtest.h>
#include <iostream>
#include <solux/util/random.h>
#include "solux/util/solux_util.h"
#include "solux/util/heap.h"
#include "solux/util/TaggedPtr.h"

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

    SOLUX_PACKED_START
    struct s2 {
      int64_t x;
      char c;
    } SOLUX_PACKED_END;

    EXPECT_EQ(9, sizeof(s2));  // make sure that the packed attribute does not pad the end

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
  solux::IndirectPQ<float, decltype(mycmp)> pq(vals, valPtrs);
  */

  {
    std::vector<float> vals = {50.0, 75.0, 25.0};
    std::vector<float*> valPtrs;
    valPtrs.resize(vals.size());

    solux::IndirectPQ<float, std::greater<>> pq(vals, valPtrs);
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
    solux::IndirectPQ<float, std::greater<>> pq(valPtrs, 0);
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
  }

  {
    int n = 100;
    solux::Rng rng;
    std::vector<int> vals(n);
    for (auto& val : vals) {
      val = rng.rint(1000000);
    }
    std::vector<int> sorted = vals;
    std::ranges::sort(sorted);

    std::vector<int*> ptrs(vals.size());
    solux::IndirectPQ<int, std::greater<>> pq(vals, ptrs, false);

    ASSERT_EQ(pq.top(), sorted[0]);

    for (auto expected : sorted) {
      ASSERT_EQ(pq.top(), expected);
      auto idx = pq.indexOfTop();
      ASSERT_EQ(vals[idx], expected);
      pq.removeTop();
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
