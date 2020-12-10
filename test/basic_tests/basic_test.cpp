
#include <gtest/gtest.h>
#include <iostream>
#include "solux/util/solux_util.h"

using namespace std;

// Basic sanity tests

TEST(basic_test, test_compiler) {
#ifdef NDEBUG
  cerr << "DEBUGGING DISABLED NDEBUG=" << NDEBUG;
#else
  cerr << "Debugging!";
#endif

  std::cerr << " __cplusplus=" << __cplusplus;
#ifdef __VERSION__
  std::cerr << " __VERSION__=" << __VERSION__;
#endif
#ifdef __GNUC__
  std::cerr << " __GNUC__=" << __GNUC__;
#endif
#ifdef _MSC_VER
  std::cerr << " _MSC_VER=" << _MSC_VER;
#endif
#ifdef __clang__
  std::cerr << " __clang__=" << __clang__;
#endif
#ifdef __linux__
  std::cerr << " __linux__=" << __linux__;
#endif
#ifdef __OPTIMIZE__
  std::cerr << " __OPTIMIZE__=" << __OPTIMIZE__;
#endif

  std::cerr << std::endl;
  std::cerr << "sizeof(std::string)==" << sizeof(std::string) << std::endl;

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
}


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
TEST(basic_test, test_address_sanitizer) {
  constexpr int size = 16;
  char onstack[size];
  char* arr = onstack;
  char* ptr = (char*)malloc(size);
  memset(ptr,'A',size);
  free(ptr);
  ptr = (char*)malloc(size);
  char x = 0;
  uint64_t off = xorshift(0) + xorshift(reinterpret_cast<uint64_t>(ptr) % 2);  // try to foil static analysis (this should be 0 though)
  x = ptr[0 + off]; // BUG: uninitialized memory read... requires memory sanitizer, not address sanitizer
  if (x==55) { // use it...
    off = 0;
  }
  memset(ptr+off,0+off,10+off); // ok, initialize buffer
  // ptr[10+off] = x; // BUG: one past end write
  // arr[10+off] = x; // BUG: one past end write on stack

  // memcpy(ptr+1+off, arr+off, size+off); // BUG: write past end with memcpy
  // memcpy(arr+1+off, ptr+off, size+off); // BUG: write past end on stack with memcpy
  // memcpy(ptr-1+off, arr+off, size+off); // BUG: write before beginning with memcpy
  // memcpy(arr-1+off, ptr+off, size+off); // BUG: write before beginning on stack with memcpy

  if (ptr[0]=='Z' || arr[0]=='Z') {cout<<"How?";}  // use the arrays to try and prevent optimizing away. clang didn't detect last underflow w/o this.

  char* ptr2 = returnsStackAddr(ptr);
  // if (*ptr2 == 'A') { cout<<"Oops, stack frame no longer exists!"<<endl; }  // BUG: use old stack frame
  // NOTE: clion set detect_stack_use_after_return=false as default for some reason.  Change it to true, and
  // both clang and g++ catch this bug.


  free(ptr); // comment out for leak test
  // free(ptr); // BUG: double free

}



TEST(basic_test, test_neq) {
    EXPECT_NE(1, 0);
    // EXPECT_EQ(1,2);
}


// test too large tokens
// test too large positions
// test position overflow
// test weird first position (lucene disallows 0)
// test that indexing exception (too large token, etc) doesn't mess up anything because of partial index (what are our invariants?)
//  things like norms we may not get to?
