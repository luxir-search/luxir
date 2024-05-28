#include <charconv>
#include "bench/solux_bench.h"
#include "test/TestIndex.h"
#include "solux/index/Inverter.h"

using namespace solux;

template <class IterType>
static void BM_IntCol(benchmark::State& state, int32_t nDocs, int32_t docDelta, int64_t maxVal, int skip) {
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
    if (skip==1) {
      while (it.next() != IntColReader::ENDDOC) {
        ret += it.value();
        count++;
      }
    } else {
      // skipping
      int32_t doc = -1;
      for(;;) {
        doc = it.advance(doc+skip);
        if (doc == IntColReader::ENDDOC) {
          break;
        }
        ret += it.value();
        count++;
      }
    }

    benchmark::DoNotOptimize(ret);
  }

  state.counters["fp"] = ret;  // sanity check.
  state.counters["count"] = count;  // sanity check.
  state.counters["rate"] = benchmark::Counter(count, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_IntColSparse(benchmark::State& state, int32_t nDocs, int32_t docDelta, int64_t maxVal, int skip) {
  BM_IntCol<IntColReader::SparseIterator>(state, nDocs, docDelta, maxVal, skip);
}
void BM_IntColBulk(benchmark::State& state, int32_t nDocs, int32_t docDelta, int64_t maxVal, int skip) {
  BM_IntCol<IntColReader::BulkIterator>(state, nDocs, docDelta, maxVal, skip);
}

// When we test sparse sets for performance, the most interesting case is when it's still a bitset in the block.
// Search code will spend much less time in very sparse sets.
constexpr int32_t nDocs = 65536;
BENCHMARK_CAPTURE(BM_IntColSparse, denseIter,   nDocs, 1, 1100, 1);
BENCHMARK_CAPTURE(BM_IntColSparse, sparseIter,  nDocs, 4, 1100, 1);
BENCHMARK_CAPTURE(BM_IntColSparse, denseSkip3,  nDocs, 1, 1100, 3);
BENCHMARK_CAPTURE(BM_IntColSparse, denseSkip27, nDocs, 1, 1100, 27);
BENCHMARK_CAPTURE(BM_IntColSparse, sparseSkip,  nDocs, 4, 1100, 1);

BENCHMARK_CAPTURE(BM_IntColBulk,   denseIter,   nDocs, 1, 1100, 1);
BENCHMARK_CAPTURE(BM_IntColBulk,   sparseIter,  nDocs, 4, 1100, 1);
BENCHMARK_CAPTURE(BM_IntColBulk,   denseSkip3,  nDocs, 1, 1100, 3);
BENCHMARK_CAPTURE(BM_IntColBulk,   denseSkip27, nDocs, 1, 1100, 27);
BENCHMARK_CAPTURE(BM_IntColBulk,   sparseSkip,  nDocs, 4, 1100, 1);
