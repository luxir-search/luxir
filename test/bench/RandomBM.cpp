#include <benchmark/benchmark.h>
#include <random>
#include "solux/util/random.h"
#include "solux/util/solux_util.h"
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

inline uint64_t mymix(uint64_t v) {
  // change to different implementations here to test mixing
  return solux::Hash::hash(&v, sizeof(uint64_t));
}

static void BM_mix(benchmark::State& state) {
  uint64_t result = 1;
  for (auto _ : state) {
    result += mymix(result);
    result *= mymix(result);
    result += mymix(result);
    result *= mymix(result);
    benchmark::DoNotOptimize(result);
    benchmark::ClobberMemory();
  }
  ASSERT_TRUE(result != 0);
}

// Only turn these on when doing random perf testing.
#ifdef RUN_DISABLED_BENCHMARKS
// Register the function as a benchmark
BENCHMARK(BM_mersenne_twister);
BENCHMARK(BM_RomuTrio);
BENCHMARK(BM_SplitMix64);
BENCHMARK(BM_mix);
#else
inline void hackety_hack() {
  solux::unused(hackety_hack);
  solux::unused(BM_mersenne_twister);  // get rid of "unused" warnings
  solux::unused(BM_RomuTrio);
  solux::unused(BM_SplitMix64);
  solux::unused(BM_mix);
}
#endif