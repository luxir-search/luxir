#include "bench/solux_bench.h"
#include "test/SegmentTest.h"


/* Results of postings reading before position blocks are supported (just reading vints directly,
 * without decoding into intermediate array:
Load Average: 0.33, 0.20, 0.13
----------------------------------------------------------------------------
Benchmark                                  Time             CPU   Iterations
----------------------------------------------------------------------------
BM_blockDecode/SimpleCodec              7.84 ns         7.84 ns     89575561
BM_blockDecode/FastPFor                  697 ns          697 ns      1009853
BM_blockDecode/SIMDFastPFor              659 ns          659 ns      1064500
BM_blockDecode/SIMDFastPForDelta1        680 ns          680 ns      1016907
BM_readAllPositions                      403 ns          403 ns      1775818 ndoc=10 npos=120
(a few more runs yielded CPU time of 415,406,412,412)
 This is faster than a block decode?! Is some of the calculation being moved outside of the loop or eliminated???
 No, even creating a new random index each time, it came out to 663ns
 Which is more realistic?
 Ahhh, pause/resume time *takes* a lot of time!!!!
 https://stackoverflow.com/questions/56660845/google-benchmark-state-pausetiming-and-state-resumetiming-take-a-long-time

 For clang 11 (instead of gcc 10.2), it looks more like this:
 Benchmark                                  Time             CPU   Iterations
----------------------------------------------------------------------------
BM_blockDecode/SimpleCodec              7.60 ns         7.60 ns     91480782
BM_blockDecode/FastPFor                  467 ns          467 ns      1506020
BM_blockDecode/SIMDFastPFor              446 ns          446 ns      1575341
BM_blockDecode/SIMDFastPForDelta1        494 ns          494 ns      1421284
BM_readAllPositions                      479 ns          479 ns      1498213 ndoc=10 npos=120

 TODO: is there a compilation issue with gcc here?  Normally it's faster than clang, so being so much slower
 is something to look into!
 */

static void BM_readAllPositions(benchmark::State& state) {
  solux::SegmentTest seg;

  seg.r.init(1);  // keep seed the same for performance benchmark
  seg.initWriter();
  seg.addFields(false, 1, 1, 10, 12); // 10 docs, 12 positions per doc (i.e. less than a block size)
  seg.initReader();
  seg.addFields(true, 1, 1, 10, 12);  // verify reading

  uint64_t ndocs=0, npos=0;
  for (auto _ : state) {
    if (solux::unit_tests) {
      // add a new different index each time if we are running unit tests
      state.PauseTiming();  // PauseTiming and ResumeTiming are very slow (~200ns)! Don't use in conjunction with anything fast!
      seg.initWriter();
      seg.addFields(false, 1, 1, 10, 12); // 10 docs, 12 positions per doc (i.e. less than a block size)
      seg.initReader();
      state.ResumeTiming();
    }

    uint64_t fingerprint = 1;
    benchmark::DoNotOptimize( std::tie(fingerprint, ndocs, npos) = seg.readFingerprint() );
    // benchmark::DoNotOptimize(fingerprint); // this causes fingerprint to be 0???? (when I tie'd directly to seg.readFingerprint()) compiler bug?
    benchmark::ClobberMemory();
    ASSERT_EQ(seg.fingerprint, fingerprint);
  }

  state.counters["ndoc"] = ndocs;
  state.counters["npos"] = npos;
}

BENCHMARK(BM_readAllPositions);
