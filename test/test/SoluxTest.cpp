#include <filesystem>
#include <thread>
#include "SoluxTest.h"
#include "GrpcSoluxTest.h"
#include "solux/solux_main.h"
#include "solux/SoluxConfig.h"
#include "benchmark/benchmark.h"


// Fix for clang/gcc linking errors.
// https://github.com/abseil/abseil-cpp/issues/1747
// https://github.com/llvm/llvm-project/issues/102443
#include "absl/base/config.h"
#include "solux/util/Signal.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {
template LogMessage& LogMessage::operator<<(const char& v);
template LogMessage& LogMessage::operator<<(const signed char& v);
template LogMessage& LogMessage::operator<<(const unsigned char& v);
template LogMessage& LogMessage::operator<<(const short& v);           // NOLINT
template LogMessage& LogMessage::operator<<(const unsigned short& v);  // NOLINT
template LogMessage& LogMessage::operator<<(const int& v);
template LogMessage& LogMessage::operator<<(const unsigned int& v);
template LogMessage& LogMessage::operator<<(const long& v);           // NOLINT
template LogMessage& LogMessage::operator<<(const unsigned long& v);  // NOLINT
template LogMessage& LogMessage::operator<<(const long long& v);      // NOLINT
template LogMessage& LogMessage::operator<<(const unsigned long long& v);  // NOLINT
template LogMessage& LogMessage::operator<<(void* const& v);
template LogMessage& LogMessage::operator<<(const void* const& v);
template LogMessage& LogMessage::operator<<(const float& v);
template LogMessage& LogMessage::operator<<(const double& v);
template LogMessage& LogMessage::operator<<(const bool& v);
}  // namespace log_internal
ABSL_NAMESPACE_END
}  // namespace absl


namespace solux {

// Global random seed that is the same for all tests in a run.
// For now, same as google's random seed used to shuffle tests, but google only uses values [0,99999] so
// we might want to be able to initialize it some other way in the future.
uint64_t SoluxTest::global_random_seed;
uint64_t SoluxTest::rng_seed;
Rng SoluxTest::rng;
SoluxNode* SoluxTest::soluxNode;

GRPCServer* GrpcSoluxTest::server = nullptr;
std::thread GrpcSoluxTest::serverThread;
std::shared_ptr<grpc::Channel> GrpcSoluxTest::channel = nullptr;

void SoluxTest::clearCollection(std::string_view collectionName) {
  auto collection = soluxNode->getCollection(collectionName);
  if (collection) {
    collection->getShard()->getIndexWriter()->testDeleteAllData();
  }
}

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

  void OnTestEnd(const testing::TestInfo &test_info) override {
    unused(test_info);
    // std::cout << "ENDING TEST " << test_info.name() << std::endl;
    assert(MemPool::sanityCheck());
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

int main(int argc, char **argv) {
  gArgc = argc;
  gArgv = argv;

  std::cout << solux_banner() << std::endl;

  spdlog::set_pattern("%L %H:%M:%S.%f T%t %s:%# %v");

  // --- CLI11 parsing for our own flags ---
  CLI::App app{"Solux unit tests and benchmarks combined.\n"
               "When running unit tests, benchmarks are also run with --benchmark_min_time=1x to make them quick.\n"
               "Run benchmarks only with normal google benchmark defaults by passing --bench.\n"
               "Even when running benchmarks only, gtest flags like --gtest_break_on_failure are still honored."};
  app.set_help_flag();  // disable built-in --help so we can handle it ourselves
  app.allow_extras();

  solux::SoluxConfig config;
  config.log_level = "debug";  // default to debug for tests
  config.server.grpc.port = 0;  // dynamic port for test server
  config.addOptions(app);

  bool help = false;
  bool bench = false;
  app.add_flag("-h,--help", help, "Print help message and exit");
  app.add_flag("--bench", bench, "Run benchmarks only (skip unit tests)");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  config.normalize();
  config.apply();

  LOG_INFO("Logging: compile-time={}, runtime={}",
           spdlog::level::to_string_view((spdlog::level::level_enum)SPDLOG_ACTIVE_LEVEL),
           spdlog::level::to_string_view(spdlog::get_level()));

  // Build argv from remaining (unrecognized) args for gtest/gbench.
  auto remaining = app.remaining();
  std::vector<char *> myargv;
  myargv.push_back(argv[0]);
  for (auto &s : remaining) {
    myargv.push_back(const_cast<char *>(s.c_str()));
  }

  if (help) {
    std::cout << app.help() << std::endl;
    myargv.push_back(const_cast<char *>("--help"));
    myargv.push_back(nullptr);
    int myargc = (int)(myargv.size() - 1);
    std::cout << "============================== Google Test Help ==============================" << std::endl;
    testing::InitGoogleTest(&myargc, &(myargv[0]));
    std::cout << "\n============================== Google Bench Help =============================" << std::endl;
    benchmark::Initialize(&myargc, &(myargv[0]));
    std::cout << std::endl;
    return 0;
  }

  // Turn on shuffling by default since that is how we get different random seeds for each run for
  // our PRNGs.
  myargv.push_back(const_cast<char *>("--gtest_shuffle"));
  myargv.push_back(nullptr);  // gtest depends on null-terminated argv
  int myargc = (int)(myargv.size() - 1);

  solux::unit_tests = !bench;

  // Init gtest so things like --gtest_break_on_failure work even in benchmark-only mode.
  testing::InitGoogleTest(&myargc, &(myargv[0]));
  myargv.resize(myargc);  // InitGoogleTest removed params it handled

  testing::AddGlobalTestEnvironment(new solux::SoluxEnvironment());

  int ret = 0;
  solux::SoluxNode node{config};
  solux::SoluxTest::soluxNode = &node;

  // Run tests / benchmarks in their own TBB arena.
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

  solux::GrpcSoluxTest::stopServer();

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

    bool hasMinTime = std::any_of(myargv.begin(), myargv.end()-1,
        [](char* s){ return std::string_view(s).starts_with("--benchmark_min_time"); });

    if (!hasMinTime && solux::unit_tests) {
      // turn down the time it takes to run tests if the benchmarks are just being run as part of unit tests
      myargv.push_back(const_cast<char *>("--benchmark_min_time=1x"));
      std::cout << "\tNOTE: setting --benchmark_min_time=1x" << std::endl;
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