#pragma once
#include <chrono>
#include "benchmark/benchmark.h"
#include "test/SoluxTest.h"

// Solux project benchmarks - always use real time since we extensively use TBB
// throughout the codebase and CPU time only measures the main thread
#define SOLUX_BENCHMARK(...) BENCHMARK(__VA_ARGS__)->UseRealTime()
#define SOLUX_BENCHMARK_CAPTURE(...) BENCHMARK_CAPTURE(__VA_ARGS__)->UseRealTime()

namespace solux {

// true if we are running benchmarks as part of unit tests (i.e. it's ok
// to do things that will mess up timings in the name of better test coverage.)
extern bool unit_tests;


///
/// A manual benchmark timing class for google benchmark.  To use it, you must turn on manual
/// timing for the benchmark:
/// BENCHMARK(BM_myBench)->UseManualTime();
///
/// DO NOT USE state.PauseTiming() and state.ResumeTiming() for anything fast, since they are very slow
/// themselves and add hundreds of nanoseconds attributed to the test.
///
/// WARNING: if you do use manual benchmark timing, the google bench test runner has an issue where
/// manual timing will be used to calculate the number of iterations (i.e. minTime), so benchmarks will
/// take a ton of time if you have a relatively slow operation outside of the manual timing measurement.
//
class BenchTimer {
  benchmark::State& state;
  std::chrono::high_resolution_clock::time_point startTime;
  double elapsed=0;
  bool running=false;

public:
  BenchTimer(benchmark::State& state, bool startNow=true) : state(state) {
    if (startNow) {
      start();
    }
  }

  ~BenchTimer() {
    stop();
    state.SetIterationTime(elapsed);
  }

  void start() {
    if (!running) {
      startTime = std::chrono::high_resolution_clock::now();
      running = true;
    }
  }

  void stop() {
    if (running) {
      auto endTime = std::chrono::high_resolution_clock::now();
      auto thisElapsed = std::chrono::duration_cast<std::chrono::duration<double>>(endTime - startTime).count();
      elapsed += thisElapsed;
      running = false;
    }
  }
};

}