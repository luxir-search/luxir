#include "SoluxTest.h"
#include "stdlib.h"

namespace solux {

// Global random seed that is the same for all tests in a run.
// For now, same as google's random seed used to shuffle tests, but google only uses values [0,99999] so
// we might want to be able to initialize it some other way in the future.
static uint64_t global_random_seed;

uint64_t SoluxTest::rng_seed;
SoluxTest::rng_type SoluxTest::rng;

class SoluxTestListener : public testing::EmptyTestEventListener {
  uint64_t suiteHash;
  uint64_t rng_seed;

  void OnTestSuiteStart(const testing::TestSuite &suite) override {
    // std::cout << "STARTING SUITE " << suite.name() << std::endl;
    suiteHash = Hash::hash(suite.name(), strlen(suite.name()));
    suiteHash += global_random_seed;
  }

  void OnTestStart(const testing::TestInfo &test_info) override {
    // std::cout << "STARTING TEST " << test_info.name() << std::endl;
    auto testnameHash = Hash::hash(test_info.name(), strlen(test_info.name()));
    rng_seed = (suiteHash << 32) +
               testnameHash;  // these are currently 32 bit hashes (from Hash::hash) so combine by shifting.
    rng_seed += global_random_seed;
    SoluxTest::init_test(rng_seed);
  }

};


class SoluxEnvironment : public testing::Environment {
public:
  ~SoluxEnvironment() override = default;

  // Override this to define how to set up the environment.
  void SetUp() override {
    // std::cout << "SoluxEnvironment:SetUp()" << std::endl;
    testing::UnitTest::GetInstance()->listeners().Append(new SoluxTestListener);
    auto gtest_random_seed = testing::UnitTest::GetInstance()->random_seed();
    global_random_seed = gtest_random_seed;
  }

  // Override this to define how to tear down the environment.
  void TearDown() override {

  }
};


} // end namespace


int main(int argc, char **argv) {
  printf("Running main() from %s\n", __FILE__);

  // Turn on shuffling by default since that is how we get different random seeds for each run for
  // our PRNGs.
  // --gtest-shuffle
  std::vector<char *> myargv(argv, argv + argc);
  myargv.push_back(const_cast<char *>("--gtest_shuffle"));
  int myargc = myargv.size();

  testing::InitGoogleTest(&myargc, &(myargv[0]));
  testing::AddGlobalTestEnvironment(new solux::SoluxEnvironment());
  return RUN_ALL_TESTS();
}
