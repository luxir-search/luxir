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

===================================== performance progression ===========================================
1: simple code that didn't handle blocks and decoded vints directly (no intermediate decode buffer): ~410ns
2: moving to always using a pos buffer (but still no block decoding): ~620ns
3: implementing block position decoding: ~1050ns (for 128 positions as opposed to previous 120)
BM_readAllPositions                     1041 ns         1041 ns       680326 ndoc=8 npos=128
****************/



static void BM_readAllPositions(benchmark::State& state) {
  solux::SegmentTest seg;

  uint32_t nDocs=8;
  // uint32_t nPos=15;  // 8 docs * 15 positions per doc (i.e. slightly less than a block size of positions)
  uint32_t nPos=16;  // 8 docs * 16 positions per doc (exactly one position block)

  seg.r.init(1);  // keep seed the same for performance benchmark
  seg.initWriter();
  seg.addFields(false, 1, 1, nDocs, nPos);
  seg.initReader();
  seg.addFields(true, 1, 1, nDocs, nPos);  // verify reading

  uint64_t ndocs=0, npos=0;
  for (auto _ : state) {
    if (solux::unit_tests) {
      // add a new different index each time if we are running unit tests
      state.PauseTiming();  // PauseTiming and ResumeTiming are very slow (~200ns)! Don't use in conjunction with anything fast!
      seg.initWriter();
      seg.addFields(false, 1, 1, nDocs, nPos);
      seg.initReader();
      state.ResumeTiming();
    }

    uint64_t fingerprint = 1;
    benchmark::DoNotOptimize( std::tie(fingerprint, ndocs, npos) = seg.readFingerprint() );
    // benchmark::DoNotOptimize(fingerprint); // this causes fingerprint to be 0???? (when I tie'd directly to seg.readFingerprint()) compiler bug?
    benchmark::ClobberMemory();
    ASSERT_EQ(seg.fingerprint, fingerprint);
    ASSERT_EQ(nDocs*nPos, npos);
  }

  state.counters["ndoc"] = ndocs;
  state.counters["npos"] = npos;
}

BENCHMARK(BM_readAllPositions);
