#include <charconv>
#include "bench/solux_bench.h"
#include "test/TestIndex.h"
#include "solux/index/Inverter.h"

using namespace solux;


static void BM_IntCol(benchmark::State& state, int32_t nDocs, int32_t docDelta, int64_t maxVal, bool advance) {
  Rng rng(0);
  test::TestIndex testIndex;
  test::TestField testField(testIndex, "foo_i");
  testField.startIndexing();
  int32_t docid = -1;
  for (;;) {
    if (docDelta != 1) {
      docid += rng.rint(1,docDelta);
    } else {
      docid++;
    }
    if (docid >= nDocs) {
      break;
    }
    testField.add(docid, rng.rint(maxVal));
  }
  testIndex.flush();
  testField.startReading();
  testField.nextSegment();
  IntColReader& intColReader = *testField.colReader;

  int64_t ret;
  int64_t count;
  for (auto _ : state) {
    ret = 0;
    count = 0;

    IntColReader::Iterator it(intColReader);
    if (!advance) {
      while (it.next() != IntColReader::ENDDOC) {
        ret += it.value();
        count++;
      }
    } else {
      // skipping
      int32_t doc = -1;
      for(;;) {
        doc = it.advance(doc+3);
        if (doc == IntColReader::ENDDOC) {
          break;
        }
        ret += it.value();
        count++;
      }
    }

    benchmark::DoNotOptimize(ret);
  }

  state.counters["count"] = count;  // sanity check.
  state.counters["rate"] = benchmark::Counter(count, benchmark::Counter::kIsIterationInvariantRate);
}


constexpr int32_t nDocs = 65536;
BENCHMARK_CAPTURE(BM_IntCol, denseIter, nDocs, 1, 1100, false);
BENCHMARK_CAPTURE(BM_IntCol, sparseIter, nDocs, 4, 1100, false);
BENCHMARK_CAPTURE(BM_IntCol, denseSkip, nDocs, 1, 1100, true);
BENCHMARK_CAPTURE(BM_IntCol, sparseSkip, nDocs, 4, 1100, true);