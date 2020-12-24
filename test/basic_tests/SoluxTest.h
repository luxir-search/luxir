#pragma once
#include <gtest/gtest.h>
#include "solux/util/solux_util.h"


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

//
// TODO: switch from std::cout to some sort of logging framework where we can disable output like this by default and
// easily enable for debugging.  Research NanoLog more... only has support for printf type formatters
// (i.e. logging a vector of string wouldn't work...) but perhaps we could come up with some macros that check if a message would be logged
// and only then generate a string from an object.  Or, start with something with better support / adoption (and Windows support), like spdlog.
// Also: investigate g3log's crash resistance / logging.
//

// Adapted from http://prng.di.unimi.it/splitmix64.c (ORIG LICENSE: CC0 / public domain)
class SplitMix64 {
  uint64_t x;
public:
  explicit SplitMix64() {} // unseeded! call init() before using.
  explicit SplitMix64(uint64_t seed) : x(seed) {}
  void init(uint64_t seed) {
    x = seed;
  }

  uint64_t operator()() {
    uint64_t z = (x += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
  }
};

// Adapted from https://www.romu-random.org/romupaper.pdf  (ORIG LICENSE: Apache 2)
// This was chosen for it's speed, relative quality, while still being easy to implement in a portable way.
class RomuTrio {
  uint64_t xState, yState, zState;

public:
  // Expert! Directly seed internal state without any mixing.  The seeds should
  // be high quality, or the first number of values should be discarded.  In all cases, there
  // should be at least one non-zero state.
  void init(uint64_t seed1, uint64_t seed2,  uint64_t seed3) {
    xState = seed1;
    yState = seed2;
    zState = seed3;
  }

  // Seed from a normal (potentially low quality) seed.  A separate PRNG is used to
  // generate a high quality internal state from the provided seed.
  void init(uint64_t seed) {
    // xState will be used unchanged to return the first value, so we should either discard the first
    // value, or use another PRNG to initialize the seed.  If initialization speed is important, we
    // really probably only need to have a good seed for xState.
    SplitMix64 seeder(seed);
    init(seeder(), seeder(), seeder());
  }

  explicit RomuTrio() {} // unseeded! call init() before using.

  explicit RomuTrio(uint64_t seed) {
    init(seed);
  }

  uint64_t operator()() {
    uint64_t xp = xState, yp = yState, zp = zState;
    xState = 15241094284759029579u * zp;
    yState = yp - xp;
    yState = std::rotl(yState, 12);
    zState = zp - yp;
    zState = std::rotl(zState, 44);
    return xp;
  }
};


// TODO: implement interfaces expected of std random engines so we can use these in conjunction with
// other standard lib random code (like distribution generation)
template <class Engine>
class SoluxRand {
  Engine engine;
public:
  SoluxRand() {
  }
  SoluxRand(uint64_t seed) : engine(seed) {
  }

  void init(uint64_t seed) {
    engine.init(seed);
  }

  uint64_t operator()() {
    return engine();
  };

  uint64_t rlong() {
    return engine();
  }

  // templates to try to work with ints, longs, signed, unsigned, w/o casting.
  template <typename T>
  T rint(T max) {
    return engine() % max;
  }

  template <typename T>
  T rint(T min, T max) {
    return (rint(max - min) + min);
  }

  bool rbool() {
    // Lowest bits can be lower quality for some engines, so use highest bit.
    // For most architectures, this is compiled to a shift, which should be the same
    // speed as a logical and.
    return ((int64_t)engine()) < 0;
  }

  uint8_t rbyte() {
    return (uint8_t)engine();
  }

  double rdouble() {
    return (engine() >> 11) * 0x1.0p-53;
  }
};


class SoluxTest : public ::testing::Test {
public:
  using rng_type = SoluxRand<RomuTrio>;
  static uint64_t rng_seed;
  static rng_type rng;

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
