#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string_view>
#include <vector>

#include "bench/solux_bench.h"
#include "solux/query/NumericRangeQuery.h"
#include "solux/util/random.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

namespace {

class NumericRangeBenchIndex {
public:
  TestIndex index;
  int32_t numDocs;

  NumericRangeBenchIndex() : numDocs(solux::unit_tests ? 50'000 : 500'000) {
    std::vector<int32_t> shuffled((size_t)numDocs);
    std::iota(shuffled.begin(), shuffled.end(), 0);
    SplitMix64 rng(0x76a5b31d);
    for (int32_t i = numDocs - 1; i > 0; i--) {
      std::swap(shuffled[(size_t)i], shuffled[(size_t)rng.rint(i + 1)]);
    }

    Inverter& inverter = index.getInverter();
    auto& correlated = inverter.getIndexHandler("range_correlated_i");
    auto& random = inverter.getIndexHandler("range_shuffled_i");
    for (int32_t doc = 0; doc < numDocs; doc++) {
      inverter.startDoc();
      correlated.index(inverter, 7 + (int64_t)doc * 2);
      random.index(inverter, 7 + (int64_t)shuffled[(size_t)doc] * 2);
      inverter.finishDoc();
    }
    index.flush();
    index.initReader();
  }
};

NumericRangeBenchIndex& benchIndex() {
  static NumericRangeBenchIndex fixture;
  return fixture;
}

void BM_NumericRangeZoneMap(benchmark::State& state, bool correlated,
                            bool pruned, int32_t perMille) {
  auto& fixture = benchIndex();
  std::string_view field = correlated
      ? "range_correlated_i" : "range_shuffled_i";
  int32_t expected = std::max<int32_t>(1,
      (int32_t)((int64_t)fixture.numDocs * perMille / 1000));
  int64_t lo = 7;
  int64_t hi = 7 + (int64_t)(expected - 1) * 2;

  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, *fixture.index.reader);
    NumericRangeQuery query(field, lo, hi);
    auto* weight = static_cast<NumericRangeQuery::Weight*>(
        query.createWeight(context, 0));
    auto& segment = fixture.index.reader->segments()[0];
    Query::Scorer* scorer = pruned
        ? weight->createScorer(pool, segment)
        : weight->createFullScanScorerForTests(pool, segment);
    int32_t count = 0;
    if (scorer != nullptr) {
      while (scorer->next() != PostingsReader::END) count++;
    }
    benchmark::DoNotOptimize(count);
    if (count != expected) {
      state.SkipWithError("numeric range benchmark match count mismatch");
      break;
    }
  }
  state.counters["docs"] = fixture.numDocs;
  state.counters["matches"] = expected;
}

} // namespace

SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, correlated_full_0_1pct,
                        true, false, 1);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, correlated_pruned_0_1pct,
                        true, true, 1);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, correlated_full_5pct,
                        true, false, 50);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, correlated_pruned_5pct,
                        true, true, 50);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, correlated_full_50pct,
                        true, false, 500);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, correlated_pruned_50pct,
                        true, true, 500);

SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, shuffled_full_0_1pct,
                        false, false, 1);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, shuffled_pruned_0_1pct,
                        false, true, 1);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, shuffled_full_5pct,
                        false, false, 50);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, shuffled_pruned_5pct,
                        false, true, 50);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, shuffled_full_50pct,
                        false, false, 500);
SOLUX_BENCHMARK_CAPTURE(BM_NumericRangeZoneMap, shuffled_pruned_50pct,
                        false, true, 500);

