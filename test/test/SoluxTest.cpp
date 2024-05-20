#include <filesystem>
#include <thread>
#include "SoluxTest.h"
#include "solux/server/GRPCServer.h"
#include "solux/solux_main.h"
#include "benchmark/benchmark.h"

namespace solux {

// Global random seed that is the same for all tests in a run.
// For now, same as google's random seed used to shuffle tests, but google only uses values [0,99999] so
// we might want to be able to initialize it some other way in the future.
uint64_t SoluxTest::global_random_seed;
uint64_t SoluxTest::rng_seed;
Rng SoluxTest::rng;
SoluxNode* SoluxTest::soluxNode;


class SoluxTestListener : public testing::EmptyTestEventListener {
  uint64_t suiteHash;
  uint64_t rng_seed;

  void OnTestSuiteStart(const testing::TestSuite &suite) override {
    // std::cout << "STARTING SUITE " << suite.name() << std::endl;
    suiteHash = Hash::hash(suite.name(), strlen(suite.name()));
    suiteHash += SoluxTest::global_random_seed;
  }

  void OnTestStart(const testing::TestInfo &test_info) override {
    // std::cout << "STARTING TEST " << test_info.name() << std::endl;
    solux::Signal::clear(); // clear all listeners before each test
    auto testnameHash = Hash::hash(test_info.name(), strlen(test_info.name()));
    rng_seed = (suiteHash << 32) +
               testnameHash;  // these are currently 32 bit hashes (from Hash::hash) so combine by shifting.
    rng_seed += SoluxTest::global_random_seed;
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
    SoluxTest::global_random_seed = gtest_random_seed;
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

int main(int argc, char **argv) {
  gArgc = argc;
  gArgv = argv;

  std::cout << solux_banner() << std::endl;

  // spdlog::set_pattern("%L %H:%M:%S.%e %s:%# %! %n T%t %v");  // this includes the method name, which messes with alignment too much
  spdlog::set_pattern("%L %H:%M:%S.%e %s:%# T%t %v");
  spdlog::set_level(spdlog::level::debug); // Set global log level to debug

  LOG_INFO("Logging: compile-time={}, runtime default={}",
           spdlog::level::level_string_views[SPDLOG_ACTIVE_LEVEL],
           spdlog::level::level_string_views[spdlog::get_level()]);

  // NOTE: argv is actually terminated by NULL!  (i.e. argv[argc]==null_ptr) And google-test actually depends on this!
  std::vector<char *> myargv(argv, argv + argc + 1);
  int myargc = myargv.size() - 1;  // minus-one because of the null terminator

  auto help = std::any_of(myargv.begin(), myargv.end()-1,  [](char* s){return strcmp(s,"--help")==0;});
  if (help) {
    std::cout << std::endl
              << "Solux unit tests and benchmarks combined.  When running unit tests, benchmarks are also" << std::endl
              << "run with a smaller minTime to make them quick.  Override this by passing --benchmark_min_time=Ns."  << std::endl
              << "One can also run benchmarks only with normal google benchmark defaults by passing --bench."  << std::endl
              << "Even when running benchmarks only, some google test flags are still honored, such as " << std::endl
              << "--gtest_break_on_failure for debugging." << std::endl
              << std::endl
              << "============================== Google Test Help ==============================" << std::endl;
              testing::InitGoogleTest(&myargc, &(myargv[0]));
    std::cout << "\n============================== Google Bench Help =============================" << std::endl;
    benchmark::Initialize(&myargc, &(myargv[0]));
    std::cout << std::endl;
    return 0;
  }

  // Turn on shuffling by default since that is how we get different random seeds for each run for
  // our PRNGs.
  myargv.back() = const_cast<char *>("--gtest_shuffle");  // overwrite the null terminator and add another
  myargv.push_back(nullptr);

  solux::unit_tests = !std::any_of(myargv.begin(), myargv.end()-1,  [](char* s){return strcmp(s,"--bench")==0;} );

  // init gtest so things like --gtest_break_on_failure work in benchmarks.
  myargc = myargv.size() - 1;  // minus-one because of the null terminator
  testing::InitGoogleTest(&myargc, &(myargv[0]));
  myargv.resize(myargc);  // InitGoogleTest removed params it handled

  testing::AddGlobalTestEnvironment(new solux::SoluxEnvironment());

  int ret = 0;
  solux::GRPCServer server;
  solux::SoluxTest::soluxNode = &server.getSoluxNode();

  // TODO: pull this out and only do it on demand if the specific test needs it?
  std::thread serverThread([&server](){server.run();});
  server.waitForStart();

  // Run tests / benchmarks in their own TBB arena.
  // It's not clear at this point if it will help anything, but we do want to separate as much as possible.
  tbb::task_arena test_arena;
  test_arena.execute([&] {
    if (!solux::unit_tests) {
      benchmark::Initialize(&myargc, &(myargv[0]));
      benchmark::RunSpecifiedBenchmarks();
      ret = testing::Test::HasFatalFailure();
    } else {
      ret = RUN_ALL_TESTS();
    }
  });

  server.shutdown();
  serverThread.join();
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
  std::vector<char *> myargv(gArgv, gArgv + gArgc + 1);

  if (solux::unit_tests) {
    std::cout << "Benchmarks being run as part of unit tests. Pass --bench to run just benchmarks with" << std::endl
              << "normal google benchmark defaults." << std::endl;

    // bool hasMinTime = std::any_of(myargv.begin(), myargv.end(),  [](char* s){return starts_with(s,"--benchmark_min_time");} );
    bool hasMinTime = std::any_of(myargv.begin(), myargv.end()-1,  [](char* s){return starts_with(s,"--benchmark_min_time");} );

    if (!hasMinTime && solux::unit_tests) {
      // turn down the time it takes to run tests if the benchmarks are just being run as part of unit tests
      myargv.push_back(const_cast<char *>("--benchmark_min_time=.01s"));
      std::cout << "\tNOTE: setting --benchmark_min_time=.01s" << std::endl;
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