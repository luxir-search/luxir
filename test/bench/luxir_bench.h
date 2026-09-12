// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <sys/resource.h>
#include <fstream>
#include <malloc.h>
#include "benchmark/benchmark.h"
#include "test/LuxirTest.h"


// Luxir project benchmarks - always use real time since we extensively use TBB
// throughout the codebase and CPU time only measures the main thread
#define LUXIR_BENCHMARK(...) BENCHMARK(__VA_ARGS__)->UseRealTime()
#define LUXIR_BENCHMARK_CAPTURE(...) BENCHMARK_CAPTURE(__VA_ARGS__)->UseRealTime()

// Measurement-only benchmarks used to choose or tune an implementation. They
// remain registered for explicit --bench runs, but Benchmarks.all excludes the
// Tuning/ namespace unless the caller supplies --benchmark_filter.
#define TUNING_BENCHMARK(...) \
  BENCHMARK(__VA_ARGS__)->Name("Tuning/" #__VA_ARGS__)
#define TUNING_BENCHMARK_CAPTURE(func, test_case_name, ...) \
  BENCHMARK_CAPTURE(func, test_case_name, __VA_ARGS__) \
      ->Name("Tuning/" #func "/" #test_case_name)

namespace luxir {

/// Match java's String.hashCode() implementation.
inline int32_t java_string_hashcode(std::string_view sv) {
  int32_t hash = 0;
  for (char c : sv) {
    hash = 31 * hash + static_cast<int32_t>(c);
  }
  return hash;
}

// true if we are running benchmarks as part of unit tests (i.e. it's ok
// to do things that will mess up timings in the name of better test coverage.)
extern bool unit_tests;

// Optional benchmark corpora can be absent on an offline or minimal setup.
// Skip with a message so Benchmarks.all still covers the available corpora.
// Returns true (and skips) when data is absent; callers should return immediately.
inline bool skipBenchIfDataMissing(benchmark::State& state, bool present, std::string_view what) {
  if (present) return false;
  state.SkipWithMessage(std::string(what) + " not available; see the test-data setup");
  return true;
}

inline size_t currentRSSKB() {
  std::ifstream statm("/proc/self/statm");
  size_t size, resident;
  statm >> size >> resident;
  size_t page_size_kb = sysconf(_SC_PAGESIZE) / 1024;
  return resident * page_size_kb;
}

inline size_t peakRSSKB() {
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  return usage.ru_maxrss;
}

class RSSWatcher {
public:
  std::thread watcherThread;
  std::atomic<bool> stopRequested = false;
  size_t sleepns;
  size_t startRSS = 0;
  size_t maxRSS = 0;
  size_t page_size_kb = 0;

  // By default sleep for 1ms between checks.
  size_t getRSSKB() {
    std::ifstream statm("/proc/self/statm");
    size_t size, resident;
    statm >> size >> resident;
    return resident * page_size_kb;
  }

  RSSWatcher(size_t sleepNs = 1000000) {
    malloc_trim(0);
    sleepns = sleepNs;
    page_size_kb = sysconf(_SC_PAGESIZE) / 1024;
    startRSS = getRSSKB();

    watcherThread = std::thread([this]() {
      poll();
    });
  }

  void poll() {
    while (!stopRequested) {
      // sleep for 1ms, then check RSS
      std::this_thread::sleep_for(std::chrono::nanoseconds(sleepns));
      size_t rss = getRSSKB();
      if (rss > maxRSS) {
        maxRSS = rss;
      }
    }
  }

  // returns delta, max
  std::pair<size_t, size_t> getDeltaKB() {
    stopRequested = true;
    if (watcherThread.joinable()) {
      watcherThread.join();
    }
    // LOG_ERROR("deltaRSS MB={} maxRSS MB={}", (maxRSS - startRSS) / 1024, maxRSS / 1024);
    return {maxRSS - startRSS, maxRSS};
  }

  ~RSSWatcher() {
    getDeltaKB();
  }
};


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

namespace test {
  class CollectionHelper;
}


void buildBenchIndex(luxir::test::CollectionHelper& helper, int64_t nDocs, std::span<const int32_t> docsPerSeg);

} // namespace luxir
