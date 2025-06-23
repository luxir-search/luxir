
#include "bench/solux_bench.h"
#include "solux/search/Collector.h"

using namespace solux;

template<typename T>
static void BM_Collector(benchmark::State& state, int32_t nDocs, bool sorted) {
  // Rng rng(SoluxTest::rng_seed);
  if (solux::unit_tests) {
    nDocs = 100; // for unit tests, use a smaller number of docs
  }
  Rng rng(1);
   uint32_t topK = state.range(0); // number of iterators / streams to merge

  double ret = 0;
  for (auto _ : state) {
    T collector(topK);
    for (int i = 0; i < nDocs; i++) {
      float score = sorted ? float(i) : float(rng());
      collector.collect(0, i, score);
    }
    collector.sort();
    ret = collector.topDocs[0].score;
    benchmark::DoNotOptimize(ret);
  }
  state.counters["topscore"] = ret;  // sanity check that different implementations are agreeing.
}

static void BM_CollectorPQ(benchmark::State& state, int32_t nDocs, bool sorted) {
  BM_Collector<TopDocsCollector>(state, nDocs, sorted);
}
static void BM_CollectorMed(benchmark::State& state, int32_t nDocs, bool sorted) {
  BM_Collector<TopScoreCollector>(state, nDocs, sorted);
}
static void BM_CollectorMedI(benchmark::State& state, int32_t nDocs, bool sorted) {
  BM_Collector<TopScoreCollectorI>(state, nDocs, sorted);
}



// #define RUN_DISABLED_BENCHMARKS
#ifdef RUN_DISABLED_BENCHMARKS
constexpr int32_t ndocs = 1000000;

BENCHMARK_CAPTURE(BM_CollectorPQ, rand, ndocs, false)->Range(1, 1<<10);
BENCHMARK_CAPTURE(BM_CollectorMed, rand, ndocs, false)->Range(1, 1<<10);
BENCHMARK_CAPTURE(BM_CollectorMedI, rand, ndocs, false)->Range(1, 1<<10);
BENCHMARK_CAPTURE(BM_CollectorPQ, sorted, ndocs, true)->Range(1, 1<<10);
BENCHMARK_CAPTURE(BM_CollectorMed, sorted, ndocs, true)->Range(1, 1<<10);
#else
inline void hackety_hack() {
  solux::unused(hackety_hack);
  solux::unused(BM_CollectorPQ);
  solux::unused(BM_CollectorMed);
  solux::unused(BM_CollectorMedI);
  solux::unused(BM_CollectorPQ);
  solux::unused(BM_CollectorMed);
}
#endif