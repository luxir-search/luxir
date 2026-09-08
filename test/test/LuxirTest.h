// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <memory>
#include <gtest/gtest.h>
#include "luxir/util/random.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

// AddressSanitizer ships its own operator new/delete; our counting override in
// LuxirTest.cpp would clash with it (alloc-dealloc-mismatch), so the override is
// compiled out under ASan and allocation counting is disabled there.
#if defined(__SANITIZE_ADDRESS__)
#  define LUXIR_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LUXIR_ASAN 1
#  endif
#endif

namespace memtrack {
// Per-thread allocation counters, updated by the global operator new/new[]
// overrides in LuxirTest.cpp. One thread-local increment per allocation. Lets a
// test assert how many heap allocations a code path makes.
// (Named memtrack, not testing, so it does not shadow gtest's ::testing.)
extern thread_local long allocCount;
extern thread_local long allocBytes;

// False under ASan (counting override disabled). Counting-based tests should
// GTEST_SKIP when this is false.
#ifdef LUXIR_ASAN
inline constexpr bool counting_enabled = false;
#else
inline constexpr bool counting_enabled = true;
#endif

// RAII window: AllocScope s; ...code...; EXPECT_EQ(0, s.count());
// Capture the delta into a local BEFORE any EXPECT (gtest macros allocate).
struct AllocScope {
  long startCount = allocCount;
  long startBytes = allocBytes;
  long count() const { return allocCount - startCount; }
  long bytes() const { return allocBytes - startBytes; }
};
}  // namespace memtrack

//
// Basic recommended test case writing:
//   class MyTest : public LuxirTest {
//   protected:
//     vector<int> vec;
//     void commonMethod();
//   }
//   TEST_F(MyTest, test1) {
//     auto val = rng.rint(10,20);  // a random number from [10,20) using our repeatable rng source
//     vec.push_back(val);
//     commonMethod();
//   }
//   TEST_F(MyTest, test2) {...}
//
// Note that a new instance of LuxirTest is created for each test (i.e. test1 and test2
// will have different instances of LuxirTest.
//
// Random numbers: use LuxirTest::rng, which is seeded with a hash on the test+suite name and
// the google test seed used to shuffle test order.  Hence to reproduce a random run, just
// use the usual google test mechanism to set it's random seed via --gtest_random_seed=...
// Note that re-running a test will result in a new seed every time since gtest shuffling
// is enabled by default.
//
// Reproducing a random sequence in a single test:
//   The simplest way is to just copy the rng.  Example:
//     auto saved_rng = rng;  // after this point, the rngs should output the same sequence of values.
//
// Debugging a failing test:
//   A standard gtest flag "--gtest_break_on_failure" will cause debuggers to suspend in any failed assertion.
//   You may want to configure your IDE to add this parameter to the test executable (and perhaps add it
//   to the Google Test template if using CLion.)


class LuxirNode;
class Schema;

class LuxirTest : public ::testing::Test {
public:
  static Rng rng;
  static uint64_t global_random_seed;  // same for all tests in a given run
  static uint64_t rng_seed;  // different for each test, but based on global_random_seed
  // Test work budget selected by --effort. Effort 1 is the quick default;
  // tests with scalable loops should derive their bounds through the helpers
  // below so effort N does roughly N times as much total work.
  static int32_t effort;
  static luxir::LuxirNode* luxirNode;
  static bool isDefaultSchema(const std::shared_ptr<Schema>& schema);

  // Scale one independent work dimension linearly with effort.
  static int64_t scaleTestWork(int64_t atEffortOne);

  // Scale one side of a multi-dimensional space so scaling every side produces
  // roughly linear total work. For example, with two dimensions and effort 4,
  // each side doubles.
  static int64_t scaleTestDimension(int64_t atEffortOne, int32_t dimensions);

  // This is called from a listener with a seed that is different for every test.
  inline static void init_test(uint64_t seed) {
    rng_seed = seed;
    rng.init(seed);
  }

  LuxirTest() {
    /*
    auto test = testing::UnitTest::GetInstance();
    auto testinfo = test->current_test_info();
    const char* name = testinfo->name();
    const char* suite_name = testinfo->test_suite_name();
    std::cout << "LuxirTest: name=" << name << " suite_name=" << suite_name  << " rng_seed=" << rng_seed << std::endl;
    */
  }

  ~LuxirTest() override {
  }

  void SetUp() override {
  }

  void TearDown() override {
  }

  void clearCollection(std::string_view collectionName="main");
};

} // end namespace
