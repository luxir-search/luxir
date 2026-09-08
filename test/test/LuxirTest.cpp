// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <thread>
#include <typeinfo>
#include "LuxirTest.h"
#include "GrpcLuxirTest.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/schema/Schema.h"
#include "luxir/luxir_main.h"
#include "luxir/LuxirConfig.h"
#include "benchmark/benchmark.h"


// Fix for clang/gcc linking errors.
// https://github.com/abseil/abseil-cpp/issues/1747
// https://github.com/llvm/llvm-project/issues/102443
#include "absl/base/config.h"
#include "luxir/util/Signal.h"

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


namespace luxir {

// Global random seed that is the same for all tests in a run.
// For now, same as google's random seed used to shuffle tests, but google only uses values [0,99999] so
// we might want to be able to initialize it some other way in the future.
uint64_t LuxirTest::global_random_seed;
uint64_t LuxirTest::rng_seed;
int32_t LuxirTest::effort = 1;
Rng LuxirTest::rng;
LuxirNode* LuxirTest::luxirNode;

GRPCServer* GrpcLuxirTest::server = nullptr;
std::thread GrpcLuxirTest::serverThread;
std::shared_ptr<grpc::Channel> GrpcLuxirTest::channel = nullptr;
bool GrpcLuxirTest::serverStartFailed = false;
bool unit_tests = true;

static bool fieldTypesEqual(const FieldType& lhs, const FieldType& rhs) {
  if (lhs.type_ != rhs.type_
      || lhs.name_ != rhs.name_
      || lhs.flags_ != rhs.flags_
      || lhs.storedResource_ != rhs.storedResource_) {
    return false;
  }
  if (auto* l = dynamic_cast<const TextFieldType*>(&lhs)) {
    auto* r = dynamic_cast<const TextFieldType*>(&rhs);
    // Component names only: the default schema's components take no parameters.
    if (r == nullptr || l->analyzer_->tokenizer->name != r->analyzer_->tokenizer->name ||
        l->analyzer_->filters.size() != r->analyzer_->filters.size()) {
      return false;
    }
    for (size_t i = 0; i < l->analyzer_->filters.size(); i++) {
      if (l->analyzer_->filters[i]->name != r->analyzer_->filters[i]->name) return false;
    }
    return true;
  }
  if (auto* l = dynamic_cast<const VectorFieldType*>(&lhs)) {
    auto* r = dynamic_cast<const VectorFieldType*>(&rhs);
    return r != nullptr
           && l->dims_ == r->dims_
           && l->metric_ == r->metric_
           && l->normalized_ == r->normalized_
           && l->normalizeOnWrite_ == r->normalizeOnWrite_;
  }
  if (auto* l = dynamic_cast<const StoredFieldType*>(&lhs)) {
    auto* r = dynamic_cast<const StoredFieldType*>(&rhs);
    return r != nullptr
           && l->codec_ == r->codec_
           && l->chunkTargetUncompressed_ == r->chunkTargetUncompressed_
           && l->maxDocsPerChunk_ == r->maxDocsPerChunk_;
  }
  return typeid(lhs) == typeid(rhs);
}

bool LuxirTest::isDefaultSchema(const std::shared_ptr<Schema>& schema) {
  if (schema == nullptr) return false;
  static const std::shared_ptr<Schema> defaultSchema = Schema::createDefaultSchema();
  if (schema->fieldTypeMap.size() != defaultSchema->fieldTypeMap.size()) return false;
  for (const auto& [name, defaultField] : defaultSchema->fieldTypeMap) {
    auto it = schema->fieldTypeMap.find(name);
    if (it == schema->fieldTypeMap.end() || !fieldTypesEqual(*it->second, *defaultField)) return false;
  }
  return true;
}

int64_t LuxirTest::scaleTestWork(int64_t atEffortOne) {
  assert(effort >= 1);
  assert(atEffortOne >= 0);
  if (atEffortOne > std::numeric_limits<int64_t>::max() / effort) {
    return std::numeric_limits<int64_t>::max();
  }
  return atEffortOne * effort;
}

int64_t LuxirTest::scaleTestDimension(int64_t atEffortOne, int32_t dimensions) {
  assert(effort >= 1);
  assert(atEffortOne >= 0);
  assert(dimensions >= 1);
  if (atEffortOne == 0) return 0;
  long double factor = std::pow((long double)effort, 1.0L / dimensions);
  long double scaled = std::ceil((long double)atEffortOne * factor);
  if (scaled >= (long double)std::numeric_limits<int64_t>::max()) {
    return std::numeric_limits<int64_t>::max();
  }
  return (int64_t)scaled;
}

void LuxirTest::clearCollection(std::string_view collectionName) {
  try {
    auto collection = luxirNode->getCollection(collectionName);
    collection->getShard()->getIndexWriter()->testDeleteAllData();
  } catch (const CollectionResolutionError&) {
  }
}

class LuxirTestListener : public testing::EmptyTestEventListener {
  uint64_t suiteHash;
  uint64_t rng_seed;

  void OnTestSuiteStart(const testing::TestSuite &suite) override {
    // std::cout << "STARTING SUITE " << suite.name() << std::endl;
    suiteHash = Hash::hash(suite.name(), strlen(suite.name()));
    suiteHash += LuxirTest::global_random_seed;
  }

  void OnTestStart(const testing::TestInfo &test_info) override {
    // std::cout << "STARTING TEST " << test_info.name() << std::endl;
    // Listeners must be purged before forcing leftover graph work to completion.
    luxir::Signal::clear();
    if (luxir::unit_tests) {
      try {
        auto collection = LuxirTest::luxirNode->getCollection("main");
        auto writer = collection->getShard()->getIndexWriter();
        // An async update still queued from the previous test can be invisible here. This is the same
        // exposure tests have today and is not worth an updateGraph.wait_for_all() on every clean check.
        bool dirty = !writer->testIsEmpty() || !LuxirTest::isDefaultSchema(collection->getSchema());
        if (dirty) {
          // Tests intentionally leave main populated; reset it silently before the next test.
          writer->testDeleteAllData();
          if (!LuxirTest::isDefaultSchema(collection->getSchema())) {
            collection->setSchema(Schema::createDefaultSchema());
          }
        }
      } catch (const CollectionResolutionError&) {
      }
    }
    auto testnameHash = Hash::hash(test_info.name(), strlen(test_info.name()));
    rng_seed = (suiteHash << 32) +
               testnameHash;  // these are currently 32 bit hashes (from Hash::hash) so combine by shifting.
    rng_seed += LuxirTest::global_random_seed;
    LuxirTest::init_test(rng_seed);
  }

  void OnTestEnd(const testing::TestInfo &test_info) override {
    unused(test_info);
    // std::cout << "ENDING TEST " << test_info.name() << std::endl;
    assert(MemPool::sanityCheck());
  }
};


class LuxirEnvironment : public testing::Environment {
public:
  ~LuxirEnvironment() override = default;

  // Override this to define how to set up the environment.
  void SetUp() override {
    // std::cout << "LuxirEnvironment:SetUp()" << std::endl;
    testing::UnitTest::GetInstance()->listeners().Append(new LuxirTestListener);
    auto gtest_random_seed = testing::UnitTest::GetInstance()->random_seed();
    LuxirTest::global_random_seed = gtest_random_seed;
  }

  // Override this to define how to tear down the environment.
  void TearDown() override {

  }
};

} // end namespace

TEST(LuxirTestHarnessTest, effortScaling) {
  struct EffortRestore {
    int32_t saved = luxir::LuxirTest::effort;
    ~EffortRestore() { luxir::LuxirTest::effort = saved; }
  } restore;

  luxir::LuxirTest::effort = 1;
  EXPECT_EQ(0, luxir::LuxirTest::scaleTestWork(0));
  EXPECT_EQ(17, luxir::LuxirTest::scaleTestWork(17));
  EXPECT_EQ(0, luxir::LuxirTest::scaleTestDimension(0, 2));
  EXPECT_EQ(100, luxir::LuxirTest::scaleTestDimension(100, 2));

  luxir::LuxirTest::effort = 4;
  EXPECT_EQ(68, luxir::LuxirTest::scaleTestWork(17));
  EXPECT_EQ(200, luxir::LuxirTest::scaleTestDimension(100, 2));
  EXPECT_EQ((std::numeric_limits<int64_t>::max)(),
            luxir::LuxirTest::scaleTestWork((std::numeric_limits<int64_t>::max)()));

  luxir::LuxirTest::effort = 8;
  EXPECT_EQ(200, luxir::LuxirTest::scaleTestDimension(100, 3));
}

// global argc/argv
int gArgc;
char** gArgv;

int main(int argc, char **argv) {
  gArgc = argc;
  gArgv = argv;

  std::cout << luxir_banner() << std::endl;

  spdlog::set_pattern("%L %H:%M:%S.%f T%t %s:%# %v");

  // --- CLI11 parsing for our own flags ---
  CLI::App app{"Luxir unit tests and benchmarks combined.\n"
               "When running unit tests, benchmarks are also run with --benchmark_min_time=1x to make them quick.\n"
               "Run benchmarks only with normal google benchmark defaults by passing --bench.\n"
               "Even when running benchmarks only, gtest flags like --gtest_break_on_failure are still honored."};
  app.set_help_flag();  // disable built-in --help so we can handle it ourselves
  app.allow_extras();

  luxir::LuxirConfig config;
  config.log_level = "debug";  // default to debug for tests
  config.server.grpc.port = 0;  // dynamic port for test server
  config.store.checked_dir.sync = "throw";  // catch missing fsyncs in tests
  config.addOptions(app);

  bool help = false;
  bool bench = false;
  app.add_flag("-h,--help", help, "Print help message and exit");
  app.add_flag("--bench", bench, "Run benchmarks only (skip unit tests)");
  app.add_option("--effort", luxir::LuxirTest::effort,
                 "Scale test work from the quick effort=1 default")
      ->check(CLI::Range(1, (std::numeric_limits<int32_t>::max)()));

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  config.normalize();
  config.apply();

  LOG_INFO("Logging: compile-time={}, runtime={}, effort={}",
           spdlog::level::to_string_view((spdlog::level::level_enum)SPDLOG_ACTIVE_LEVEL),
           spdlog::level::to_string_view(spdlog::get_level()),
           luxir::LuxirTest::effort);

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

  luxir::unit_tests = !bench;

  // Init gtest so things like --gtest_break_on_failure work even in benchmark-only mode.
  testing::InitGoogleTest(&myargc, &(myargv[0]));
  myargv.resize(myargc);  // InitGoogleTest removed params it handled

  testing::AddGlobalTestEnvironment(new luxir::LuxirEnvironment());

  int ret = 0;
  luxir::LuxirNode node{config};
  luxir::LuxirTest::luxirNode = &node;

  // Run tests / benchmarks in their own TBB arena.
  tbb::task_arena test_arena;
  test_arena.execute([&] {
    if (!luxir::unit_tests) {
      benchmark::Initialize(&myargc, &(myargv[0]));
      benchmark::RunSpecifiedBenchmarks();
      ret = testing::Test::HasFatalFailure();
    } else {
      ret = RUN_ALL_TESTS();
    }
  });

  luxir::GrpcLuxirTest::stopServer();

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

  if (luxir::unit_tests) {
    std::cout << "Benchmarks being run as part of unit tests. Pass --bench to run just benchmarks with" << std::endl
              << "normal google benchmark defaults." << std::endl;

    bool hasMinTime = std::any_of(myargv.begin(), myargv.end(),
        [](char* s){ return std::string_view(s).starts_with("--benchmark_min_time"); });
    bool hasFilter = std::any_of(myargv.begin(), myargv.end(),
        [](char* s){ return std::string_view(s).starts_with("--benchmark_filter"); });

    if (!hasMinTime && luxir::unit_tests) {
      // turn down the time it takes to run tests if the benchmarks are just being run as part of unit tests
      myargv.push_back(const_cast<char *>("--benchmark_min_time=1x"));
      std::cout << "\tNOTE: setting --benchmark_min_time=1x" << std::endl;
    }

    if (!hasFilter) {
      // Tuning benchmarks only measure competing implementations. They remain
      // available to --bench and to an explicit benchmark filter.
      myargv.push_back(const_cast<char *>("--benchmark_filter=-^Tuning/"));
      std::cout << "\tNOTE: excluding Tuning/ benchmarks" << std::endl;
    }

    // TODO: should we use file instead of console output when running benchmarks as a unit test?
  }

  myargv.push_back(nullptr);
  int myargc = (int)myargv.size() - 1;
  benchmark::Initialize(&myargc, &(myargv[0]));
  benchmark::RunSpecifiedBenchmarks();
}


// Global operator new/delete overrides for the test+benchmark binary. Every
// allocation bumps a per-thread counter (see LuxirTest.h / AllocScope), which
// lets tests assert "this code path performs N allocations" - notably the
// allocation-free Unicode segmentation check. Cost is one thread-local increment
// per allocation; BM_AllocSmall_std measures it. Under MEM_SCRIBBLE we also
// scribble freshly-allocated bytes (heap use-before-init aid).
namespace luxir::memtrack {
thread_local long allocCount = 0;
thread_local long allocBytes = 0;
}

#ifndef LUXIR_ASAN  // under ASan, ASan owns operator new/delete (see LuxirTest.h)
static inline void* luxirTrackAlloc(std::size_t n, std::size_t align) {
  ++luxir::memtrack::allocCount;
  luxir::memtrack::allocBytes += (long) n;
  void* p;
  if (align <= alignof(std::max_align_t)) {
    p = std::malloc(n ? n : 1);
  } else {
    std::size_t sz = (n + align - 1) & ~(align - 1);  // aligned_alloc needs size % align == 0
    p = std::aligned_alloc(align, sz ? sz : align);
  }
  if (!p) throw std::bad_alloc();
#ifdef MEM_SCRIBBLE
  std::memset(p, 'z', n);
#endif
  return p;
}

void* operator new(std::size_t n) { return luxirTrackAlloc(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return luxirTrackAlloc(n, alignof(std::max_align_t)); }
void* operator new(std::size_t n, std::align_val_t a) { return luxirTrackAlloc(n, (std::size_t) a); }
void* operator new[](std::size_t n, std::align_val_t a) { return luxirTrackAlloc(n, (std::size_t) a); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif  // !LUXIR_ASAN
