#pragma once
#include <gtest/gtest.h>
#include "solux/util/solux_util.h"
#include "solux/util/random.h"

namespace solux {

//
// Basic recommended test case writing:
//   class MyTest : public SoluxTest {
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
// Note that a new instance of SoluxTest is created for each test (i.e. test1 and test2
// will have different instances of SoluxTest.
//
// Random numbers: use SoluxTest::rng, which is seeded with a hash on the test+suite name and
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



//
// TODO: switch from std::cout to some sort of logging framework where we can disable output like this by default and
// easily enable for debugging.  Research NanoLog more... only has support for printf type formatters
// (i.e. logging a vector of string wouldn't work...) but perhaps we could come up with some macros that check if a message would be logged
// and only then generate a string from an object.  Or, start with something with better support / adoption (and Windows support), like spdlog.
// Also: investigate g3log's crash resistance / logging.
//



class SoluxTest : public ::testing::Test {
public:
  static Rng rng;
  static uint64_t rng_seed;

  // This is called from a listener with a seed that is different for every test.
  inline static void init_test(uint64_t seed) {
    rng_seed = seed;
    rng.init(seed);
  }

  SoluxTest() {
    /***
    auto test = testing::UnitTest::GetInstance();
    auto testinfo = test->current_test_info();
    const char* name = testinfo->name();
    const char* suite_name = testinfo->test_suite_name();
    std::cout << "SoluxTest: name=" << name << " suite_name=" << suite_name  << " rng_seed=" << rng_seed << std::endl;
     ***/
  }

  ~SoluxTest() override {
  }

  void SetUp() override {
  }

  void TearDown() override {
  }

};

} // end namespace
