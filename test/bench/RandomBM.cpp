#include <benchmark/benchmark.h>
#include <random>
#include "solux/util/random.h"
#include "gtest/gtest.h"


template <class Rng>
inline uint64_t calcSimple(Rng& rng) {
  return rng();
};

template <class Rng>
inline uint64_t calcComplex(Rng& rng) {
  // do something more complex to see how the complexity of the rng can be
  // optimized.
  uint64_t v = rng();
  uint64_t x = rng();
  if ((v & 0xff) < 100) {
    v += rng() - x;
    x -= rng() + v;
  } else {
    v += (rng() + x) & 0xff;
    x += rng() - v;
  }
  v -= rng() + x;

  return v;
};

void tstz(uint64_t v) {
  ASSERT_EQ(v, 0);  // test assertions in benchmarks
}

template <class Rng>
inline void benchRng(benchmark::State& state, Rng& rng) {
  uint64_t result = 0;
  for (auto _ : state) {
    result += calcComplex(rng);
    result += calcSimple(rng);
    /*** Test failures in benchmarking (from a utility method where a return won't break the loop and cause failure.
    std::cout << "Test::HasFatalFailure=" << testing::Test::HasFatalFailure() << std::endl;
    tstz(result);
    std::cout << "Test::HasFatalFailure=" << testing::Test::HasFatalFailure() << std::endl;
    ***/
    benchmark::DoNotOptimize(result);
    benchmark::ClobberMemory();
  }
};

static void BM_mersenne_twister(benchmark::State& state) {
  std::mt19937_64 rng(1234);
  benchRng(state, rng);
}

static void BM_RomuTrio(benchmark::State& state) {
  using rng_type = solux::SoluxRand<solux::RomuTrio>;
  rng_type rng(1234);
  benchRng(state, rng);
}


static void BM_SplitMix64(benchmark::State& state) {
  using rng_type = solux::SoluxRand<solux::SplitMix64>;
  rng_type rng(1234);
  benchRng(state, rng);
}


// Register the function as a benchmark
BENCHMARK(BM_mersenne_twister);
BENCHMARK(BM_RomuTrio);
BENCHMARK(BM_SplitMix64);

// Run the benchmark
// BENCHMARK_MAIN();