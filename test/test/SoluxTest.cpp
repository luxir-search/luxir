#include "SoluxTest.h"
#include "stdlib.h"
#include "benchmark/benchmark.h"

namespace solux {

// Global random seed that is the same for all tests in a run.
// For now, same as google's random seed used to shuffle tests, but google only uses values [0,99999] so
// we might want to be able to initialize it some other way in the future.
static uint64_t global_random_seed;

uint64_t SoluxTest::rng_seed;
Rng SoluxTest::rng;

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

bool unit_tests = true;
} // end namespace

// global argc/argv
int gArgc;
char** gArgv;

bool starts_with(const char * str, const char * prefix)
{
  while (*prefix != 0) {
    if (*prefix++ != *str++) {
      return false;
    }
  }
  return true;
}

void print_env() {
  // TODO: move some of this to main() so it always appears at the start.
  std::cout << "ENV_INFO:";
#ifdef NDEBUG
  std::cout << " Release (NDEBUG)";
#else
  std::cout << " Debugging!";
#endif
#ifdef __OPTIMIZE__
  std::cout << " __OPTIMIZE__=" << __OPTIMIZE__;
#endif
  std::cout << " __cplusplus=" << __cplusplus;
#ifdef __clang__
  std::cout << " __clang__=" << __clang__;
#endif
#ifdef __GNUC__
  std::cout << " __GNUC__=" << __GNUC__;
#endif
#ifdef _MSC_VER
  std::cout << " _MSC_VER=" << _MSC_VER;
#endif
#ifdef __VERSION__
  std::cout << " __VERSION__=" << __VERSION__;
#endif
#ifdef __linux__
  std::cout << " __linux__=" << __linux__;
#endif

  // std::cout << "sizeof(std::string)==" << sizeof(std::string) << std::endl;
  std::cout << std::endl;
}

int main(int argc, char **argv) {
  gArgc = argc;
  gArgv = argv;

  print_env();

  std::vector<char *> myargv(gArgv, gArgv + gArgc);

  auto help = std::any_of(myargv.begin(), myargv.end(),  [](char* s){return strcmp(s,"--help")==0;});
  if (help) {
    std::cout << "Solux unit tests and benchmarks combined.  When running unit tests, benchmarks are also" << std::endl
              << "run with a smaller minTime to make them quick.  Override this by passing --benchmark_min_time=N."  << std::endl
              << "One can also run benchmarks only with normal google benchmark defaults by passing --bench."  << std::endl
              << "Even when running benchmarks only, some google test flags are still honored, such as " << std::endl
              << "--gtest_break_on_failure for debugging." << std::endl
              << std::endl;
  }

  // Turn on shuffling by default since that is how we get different random seeds for each run for
  // our PRNGs.
  // --gtest-shuffle
  myargv.push_back(const_cast<char *>("--gtest_shuffle"));
  solux::unit_tests = !std::any_of(myargv.begin(), myargv.end(),  [](char* s){return strcmp(s,"--bench")==0;} );
  int myargc = myargv.size();

  // init gtest so things like --gtest_break_on_failure work in benchmarks.
  testing::InitGoogleTest(&myargc, &(myargv[0]));
  myargv.resize(myargc);  // InitGoogleTest removed params it handled

  testing::AddGlobalTestEnvironment(new solux::SoluxEnvironment());

  if (!solux::unit_tests) {
    benchmark::Initialize(&myargc, &(myargv[0]));
    benchmark::RunSpecifiedBenchmarks();
    return testing::Test::HasFatalFailure();
  }

  auto ret = RUN_ALL_TESTS();
  return ret;
}

/** google benchmark arg reference
                   [--benchmark_list_tests={true|false}]
                   [--benchmark_filter=<regex>]
                   [--benchmark_min_time=<min_time>]
                   [--benchmark_repetitions=<num_repetitions>]
                   [--benchmark_report_aggregates_only={true|false}]
                   [--benchmark_display_aggregates_only={true|false}]
                   [--benchmark_format=<console|json|csv>]
                   [--benchmark_out=<filename>]
                   [--benchmark_out_format=<json|console|csv>]
                   [--benchmark_color={auto|true|false}]
                   [--benchmark_counters_tabular={true|false}]
                   [--v=<verbosity>]
 */



// unit test that runs all benchmarks at a faster speed
TEST(Benchmarks, all) {
  // if we are running unit tests, we want the benchmarks to run faster
  std::vector<char *> myargv(gArgv, gArgv + gArgc);

  if (solux::unit_tests) {
    std::cout << "Benchmarks being run as part of unit tests. Pass --bench to run just benchmarks with" << std::endl
              << "normal google benchmark defaults." << std::endl;

    // bool hasMinTime = std::any_of(myargv.begin(), myargv.end(),  [](char* s){return starts_with(s,"--benchmark_min_time");} );
    bool hasMinTime = std::any_of(myargv.begin(), myargv.end(),  [](char* s){return starts_with(s,"--benchmark_min_time");} );

    if (!hasMinTime && solux::unit_tests) {
      // turn down the time it takes to run tests if the benchmarks are just being run as part of unit tests
      myargv.push_back(const_cast<char *>("--benchmark_min_time=.01"));
      std::cout << "\tNOTE: setting --benchmark_min_time=.01" << std::endl;
    }

    // TODO: should we use file instead of console output when running benchmarks as a unit test?
  }

  int myargc = myargv.size();
  benchmark::Initialize(&myargc, &(myargv[0]));
  benchmark::RunSpecifiedBenchmarks();
}


#ifdef MEM_SCRIBBLE
void* operator new (std::size_t count ) {
  // std::cout << "new(" << count << ")" << std::endl;
  auto p = malloc(count);
  memset(p, 'z', count);
  return p;
}

void operator delete  (void* ptr) {
  // std::cout << "delete(" << ptr << ")" << std::endl;
  free(ptr);
}
void operator delete  (void* ptr, std::size_t sz) {
  // std::cout << "delete2(" << ptr << "," << sz << ")" << std::endl;
  memset(ptr, 'z', sz);
  free(ptr);
}
#endif